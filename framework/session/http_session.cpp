#include "http_session.hpp"

#include "context/http_context.hpp"
#include <fmt/core.h>
#include <utility>


using namespace khttpd::framework;

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
    fmt::print(stderr, "HttpSession on_read error: {}\n", ec.message());
    return;
  }

  if (beast::websocket::is_upgrade(req_))
  {
    fmt::print("Detected WebSocket upgrade request for target: {}\n", req_.target());
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
static std::string extract_path_from_target(std::string_view target)
{
  auto qpos = target.find('?');
  if (qpos != std::string_view::npos)
  {
    return std::string(target.substr(0, qpos));
  }
  return std::string(target);
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
    fmt::print(stderr, "Error canonicalizing path '{}': {}\n", full_local_path.string(), ec.message());
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
  const std::string& full_path_str = full_local_path.string();
  const std::string& root_path_str = canonical_web_root_path_.string();
  if (full_path_str.size() < root_path_str.size() ||
      full_path_str.substr(0, root_path_str.size()) != root_path_str)
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
      fmt::print(stderr, "Error checking if path is directory '{}': {}\n", full_local_path.string(), ec.message());
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
    fmt::print(stderr, "Error opening file {}: {}\n", full_local_path.string(), ec.message());
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

  send_response(std::move(file_res));
  return true;
}

void HttpSession::send_chunked_response()
{
  res_.body() = "";
  sr_.emplace(res_);

  // Initialize chunked writing state
  chunk_queue_ = std::make_shared<std::queue<std::string>>();
  chunk_mtx_ = std::make_shared<std::mutex>();
  chunk_writing_ = std::make_shared<bool>(false);
  chunk_error_ = std::make_shared<beast::error_code>();

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
    fmt::print(stderr, "HttpSession on_write_header error: {}\n", ec.message());
    return;
  }

  // Async chunk writer: posts each chunk write to the io_context executor.
  // The WriteHandler synchronously waits for the async write to complete
  // so the user's HttpStreamHandler can use a simple synchronous loop.
  auto write_chunk = [this](const std::string& buffer) -> bool
  {
    struct WriteState
    {
      std::mutex mtx;
      std::condition_variable cv;
      beast::error_code ec;
      bool done = false;
    };
    auto state = std::make_shared<WriteState>();

    // Build chunk: hex-length \r\n body \r\n
    std::stringstream ss;
    ss << std::hex << buffer.length() << "\r\n" << buffer << "\r\n";
    auto data = std::make_shared<std::string>(ss.str());

    // Post async write to executor
    net::post(stream_.get_executor(),
              [self = shared_from_this(), data, state]()
              {
                net::async_write(self->stream_, net::buffer(*data),
                                 [state](beast::error_code ec, std::size_t)
                                 {
                                   std::unique_lock<std::mutex> lock{state->mtx};
                                   state->ec = ec;
                                   state->done = true;
                                   state->cv.notify_one();
                                 });
              });

    // Wait for async write to complete
    std::unique_lock<std::mutex> lock{state->mtx};
    state->cv.wait(lock, [&state] { return state->done; });

    if (state->ec)
    {
      fmt::print(stderr, "Chunked write error: {}\n", state->ec.message());
      return false;
    }
    return true;
  };

  // Invoke the user's stream handler with our async-backed WriteHandler
  if (ctx->get_stream_handler())
  {
    ctx->get_stream_handler()(*ctx, write_chunk);
  }

  do_write_final_chunk();
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
    fmt::print(stderr, "HttpSession on_write error: {}\n", ec.message());
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
    fmt::print(stderr, "HttpSession shutdown error: {}\n", ec.message());
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
