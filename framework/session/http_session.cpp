#include "http_session.hpp"

#include "context/http_context.hpp"
#include <fmt/core.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <thread>
#include <utility>


using namespace khttpd::framework;

struct HttpSession::ChunkWriteState
{
  std::queue<std::string> queue;
  std::mutex mutex;
  bool writing = false;
  bool final_queued = false;
  bool completed = false;
  beast::error_code error;
  net::executor_work_guard<beast::tcp_stream::executor_type> guard;

  explicit ChunkWriteState(beast::tcp_stream::executor_type executor)
    : guard(executor)
  {
  }
};

HttpSession::HttpSession(tcp::socket&& socket, HttpRouter& router, WebsocketRouter& ws_router,
                         const std::string& web_root,
                         const boost::filesystem::path& canonical_web_root)
  : stream_(std::move(socket)),
    router_(router),
    websocket_router_(ws_router),
    web_root_path_(web_root),
    canonical_web_root_path_(canonical_web_root)
{
  if (canonical_web_root_path_.empty())
  {
    disable_web_root_ = true;
  }
}

void HttpSession::run()
{
  net::dispatch(stream_.get_executor(),
                beast::bind_front_handler(&HttpSession::do_read, shared_from_this()));
}

void HttpSession::do_read()
{
  req_ = {};
  http::async_read(stream_, buffer_, req_,
                   beast::bind_front_handler(&HttpSession::on_read, shared_from_this()));
}

void HttpSession::on_read(const beast::error_code& ec, std::size_t bytes_transferred)
{
  boost::ignore_unused(bytes_transferred);

  if (ec == http::error::end_of_stream)
  {
    return do_close();
  }
  if (ec)
  {
    spdlog::error("HttpSession on_read error: {}", ec.message());
    return;
  }

  if (beast::websocket::is_upgrade(req_))
  {
    spdlog::debug("Detected WebSocket upgrade request for target: {}", std::string(req_.target()));
    handle_websocket_upgrade();
    return;
  }

  handle_request();
}

void HttpSession::handle_request()
{
  res_ = {};

  ctx = std::make_shared<HttpContext>(req_, res_);

  try
  {
    // 1. Run Pre-interceptors
    if (router_.run_pre_interceptors(*ctx) == InterceptorResult::Stop)
    {
      router_.run_post_interceptors(*ctx);

      if (res_.chunked())
      {
        send_chunked_response();
      }
      else
      {
        send_response(std::move(res_));
      }
      return;
    }

    bool static_file_served = false;
    // 2. Dispatch to routes or static files
    router_.dispatch(*ctx, [this, &static_file_served]
    {
      if (req_.method() == http::verb::get || req_.method() == http::verb::head)
      {
        static_file_served = do_serve_static_file();
      }
      return static_file_served;
    });

    if (static_file_served)
    {
      return;
    }

    // 3. Run Post-interceptors
    router_.run_post_interceptors(*ctx);

    if (res_.chunked())
    {
      send_chunked_response();
    }
    else
    {
      send_response(std::move(res_));
    }
  }
  catch (...)
  {
    router_.handle_exception(std::current_exception(), *ctx);
    send_response(std::move(res_));
  }
}

// Extract path from request target (query-stripped)
namespace
{
  std::string extract_path_from_target(std::string_view target)
  {
    auto qpos = target.find('?');
    if (qpos != std::string_view::npos)
    {
      return std::string(target.substr(0, qpos));
    }
    return std::string(target);
  }

  bool is_path_within_root(const boost::filesystem::path& candidate,
                           const boost::filesystem::path& root)
  {
    auto root_it = root.begin();
    auto candidate_it = candidate.begin();

    for (; root_it != root.end(); ++root_it, ++candidate_it)
    {
      if (candidate_it == candidate.end() || *root_it != *candidate_it)
      {
        return false;
      }
    }

    return true;
  }
}

bool HttpSession::do_serve_static_file()
{
  if (canonical_web_root_path_.empty())
  {
    return false;
  }

  std::string request_path_str = extract_path_from_target(req_.target());
  boost::filesystem::path request_path(request_path_str);

  if (request_path == "/")
  {
    request_path = "/index.html";
  }

  boost::filesystem::path full_local_path = web_root_path_ / request_path.relative_path();
  boost::system::error_code ec;

  // 1. Normalize path to prevent directory traversal
  full_local_path = boost::filesystem::canonical(full_local_path, ec);

  if (ec)
  {
    if (ec == boost::system::errc::no_such_file_or_directory)
    {
      return false;
    }
    spdlog::error("Error canonicalizing path '{}': {}", full_local_path.string(), ec.message());
    http::response<http::string_body> forbidden_res{http::status::forbidden, req_.version()};
    forbidden_res.keep_alive(req_.keep_alive());
    forbidden_res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    forbidden_res.set(http::field::content_type, "text/html");
    forbidden_res.body() = fmt::format("<h1>403 Forbidden</h1><p>Access denied due to invalid path.</p>");
    forbidden_res.prepare_payload();
    send_response(std::move(forbidden_res));
    return true;
  }

  // 2. Security: ensure path is within web root
  if (!is_path_within_root(full_local_path, canonical_web_root_path_))
  {
    http::response<http::string_body> forbidden_res{http::status::forbidden, req_.version()};
    forbidden_res.keep_alive(req_.keep_alive());
    forbidden_res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    forbidden_res.set(http::field::content_type, "text/html");
    forbidden_res.body() = fmt::format("<h1>403 Forbidden</h1><p>Access denied: Path traversal attempt detected.</p>");
    forbidden_res.prepare_payload();
    send_response(std::move(forbidden_res));
    return true;
  }

  // 3. Check if directory
  if (boost::filesystem::is_directory(full_local_path, ec))
  {
    if (ec)
    {
      spdlog::error("Error checking if path is directory '{}': {}", full_local_path.string(), ec.message());
      return false;
    }
    boost::filesystem::path index_file_path = full_local_path / "index.html";
    if (boost::filesystem::is_regular_file(index_file_path, ec))
    {
      full_local_path = index_file_path;
    }
    else
    {
      http::response<http::string_body> forbidden_res{http::status::forbidden, req_.version()};
      forbidden_res.keep_alive(req_.keep_alive());
      forbidden_res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
      forbidden_res.set(http::field::content_type, "text/html");
      forbidden_res.body() = fmt::format("<h1>403 Forbidden</h1><p>Directory listing not allowed.</p>");
      forbidden_res.prepare_payload();
      send_response(std::move(forbidden_res));
      return true;
    }
  }

  // 4. Final check: regular file
  if (!boost::filesystem::is_regular_file(full_local_path, ec) || ec)
  {
    return false;
  }

  // 5. Serve file
  http::response<http::file_body> file_res;
  file_res.version(req_.version());
  file_res.keep_alive(req_.keep_alive());
  file_res.result(http::status::ok);
  file_res.set(http::field::server, BOOST_BEAST_VERSION_STRING);

  file_res.body().open(full_local_path.string().c_str(), beast::file_mode::scan, ec);
  if (ec)
  {
    spdlog::error("Error opening file {}: {}", full_local_path.string(), ec.message());
    http::response<http::string_body> internal_error_res{http::status::internal_server_error, req_.version()};
    internal_error_res.keep_alive(req_.keep_alive());
    internal_error_res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    internal_error_res.set(http::field::content_type, "text/html");
    internal_error_res.body() = "<h1>500 Internal Server Error</h1><p>Could not open the requested file.</p>";
    internal_error_res.prepare_payload();
    send_response(std::move(internal_error_res));
    return true;
  }

  std::string extension = full_local_path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
  file_res.set(http::field::content_type, mime_type_from_extension(extension));

  file_res.prepare_payload();

  if (req_.method() == http::verb::head)
  {
    http::response<http::empty_body> head_res{http::status::ok, req_.version()};
    head_res.keep_alive(req_.keep_alive());
    head_res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    head_res.set(http::field::content_type, mime_type_from_extension(extension));
    head_res.content_length(file_res.body().size());
    send_response(std::move(head_res));
    return true;
  }

  send_response(std::move(file_res));
  return true;
}

void HttpSession::send_chunked_response()
{
  res_.body() = "";
  sr_.emplace(res_);

  http::async_write_header(stream_, *sr_,
                           beast::bind_front_handler(
                             &HttpSession::on_write_header,
                             shared_from_this()));
}

void HttpSession::send_response(http::message_generator msg)
{
  bool keep_alive = msg.keep_alive();
  beast::async_write(stream_, std::move(msg),
                     beast::bind_front_handler(&HttpSession::on_write, shared_from_this(), keep_alive));
}

void HttpSession::on_write_header(beast::error_code ec, std::size_t bytes_transferred)
{
  boost::ignore_unused(bytes_transferred);
  if (ec)
  {
    spdlog::error("HttpSession on_write_header error: {}", ec.message());
    return;
  }

  // The stream handler may run blocking user code, while writes are serialized
  // back onto the stream executor to preserve Beast's async_write contract.
  auto self = shared_from_this();
  auto state = std::make_shared<ChunkWriteState>(stream_.get_executor());

  auto schedule_write = [self, state]()
  {
    bool should_start = false;
    {
      std::unique_lock<std::mutex> lock{state->mutex};
      if (!state->writing)
      {
        state->writing = true;
        should_start = true;
      }
    }
    if (should_start)
    {
      net::post(self->stream_.get_executor(), [self, state]()
      {
        self->do_write_chunk(state);
      });
    }
  };

  auto write_chunk = [state, schedule_write](const std::string& buffer) -> bool
  {
    std::stringstream ss;
    ss << std::hex << buffer.length() << "\r\n" << buffer << "\r\n";

    {
      std::unique_lock<std::mutex> lock{state->mutex};
      if (state->error)
      {
        return false;
      }
      state->queue.push(ss.str());
    }
    schedule_write();
    return true;
  };

  std::thread([self, state, write_chunk, schedule_write]()
  {
    if (self->ctx->get_stream_handler())
    {
      self->ctx->get_stream_handler()(*self->ctx, write_chunk);
    }

    {
      std::unique_lock<std::mutex> lock{state->mutex};
      state->queue.push("0\r\n\r\n");
      state->final_queued = true;
    }
    schedule_write();
  }).detach();
}

void HttpSession::do_write_chunk(std::shared_ptr<ChunkWriteState> state)
{
  std::shared_ptr<std::string> data;
  {
    std::unique_lock<std::mutex> lock{state->mutex};
    if (state->queue.empty())
    {
      state->writing = false;
      if (state->final_queued && !state->completed)
      {
        state->completed = true;
        lock.unlock();
        state->guard.reset();
        on_write(res_.keep_alive(), {}, 0);
      }
      return;
    }

    data = std::make_shared<std::string>(std::move(state->queue.front()));
    state->queue.pop();
  }

  net::async_write(stream_, net::buffer(*data),
                   [self = shared_from_this(), state, data](beast::error_code ec, std::size_t bytes)
                   {
                     if (ec)
                     {
                       {
                         std::unique_lock<std::mutex> lock{state->mutex};
                         state->error = ec;
                         state->writing = false;
                         state->completed = true;
                       }
                       state->guard.reset();
                       self->on_write(self->res_.keep_alive(), ec, bytes);
                       return;
                     }
                     self->do_write_chunk(state);
                   });
}

void HttpSession::do_write_final_chunk()
{
  net::async_write(stream_, net::buffer("0\r\n\r\n"),
                   beast::bind_front_handler(
                     &HttpSession::on_shutdown,
                     shared_from_this(), res_.keep_alive()));
}

void HttpSession::on_shutdown(bool keep_alive, beast::error_code ec, std::size_t bytes_transferred)
{
  on_write(keep_alive, ec, bytes_transferred);
}

void HttpSession::on_write(bool keep_alive, beast::error_code ec, std::size_t bytes_transferred)
{
  boost::ignore_unused(bytes_transferred);

  if (ec)
  {
    spdlog::error("HttpSession on_write error: {}", ec.message());
    return;
  }

  if (!keep_alive)
  {
    return do_close();
  }

  do_read();
}

void HttpSession::do_close()
{
  beast::error_code ec;
  stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
  if (ec)
  {
    spdlog::error("HttpSession shutdown error: {}", ec.message());
  }
}

void HttpSession::handle_websocket_upgrade()
{
  ws_session_ = std::make_shared<WebsocketSession>(stream_.release_socket(), websocket_router_,
                                                   std::string(req_.target()));

  ws_session_->run_handshake(req_);
}


// 辅助函数：根据文件扩展名获取 MIME 类型
std::string HttpSession::mime_type_from_extension(const std::string& ext)
{
  if (ext == ".html" || ext == ".htm") return "text/html";
  if (ext == ".css") return "text/css";
  if (ext == ".js") return "application/javascript";
  if (ext == ".json") return "application/json";
  if (ext == ".png") return "image/png";
  if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
  if (ext == ".gif") return "image/gif";
  if (ext == ".svg") return "image/svg+xml";
  if (ext == ".pdf") return "application/pdf";
  if (ext == ".txt") return "text/plain";
  return "application/octet-stream";
}
