#include "http_session.hpp"

#include "context/http_context.hpp"
#include <boost/asio/bind_cancellation_slot.hpp>
#include <fmt/core.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <thread>
#include <utility>
#include <limits>


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

class HttpSession::RequestStreamImpl final : public HttpRequestStream
{
  std::shared_ptr<HttpSession> session_;
public:
  explicit RequestStreamImpl(const std::shared_ptr<HttpSession>& session) : session_(session) {}
  void async_read_some(net::mutable_buffer target, ReadCallback callback) override
  {
    if (session_) session_->async_read_stream_body(target, std::move(callback));
    else callback(net::error::operation_aborted, 0, true);
  }
  void cancel() override
  {
    if (session_) session_->cancel_session();
  }
  void cancel_read() override { if (session_) session_->cancel_stream_body(); }
};

class HttpSession::ResponseStreamImpl final : public HttpResponseStream
{
  std::shared_ptr<HttpSession> session_;
public:
  explicit ResponseStreamImpl(const std::shared_ptr<HttpSession>& session) : session_(session) {}
  void async_start(ResponseHead head, Callback cb) override
  { if (session_) session_->start_stream_response(std::move(head), std::move(cb)); else cb(net::error::operation_aborted); }
  void async_write_some(net::const_buffer b, Callback cb) override
  { if (session_) session_->write_stream_response(b, std::move(cb)); else cb(net::error::operation_aborted); }
  void async_finish(Callback cb) override
  { if (session_) session_->finish_stream_response(std::move(cb)); else cb(net::error::operation_aborted); }
  void async_wait_disconnect(Callback cb) override
  { if (session_) session_->wait_stream_disconnect(std::move(cb)); }
  void cancel_disconnect_wait() override
  { if (session_) session_->cancel_stream_disconnect_wait(); }
  void cancel_request_body() override { if (session_) session_->cancel_stream_body(); }
  void cancel() override { if (session_) session_->cancel_session(); }
};

HttpSession::HttpSession(tcp::socket&& socket, HttpRouter& router, WebsocketRouter& ws_router,
                         const std::string& web_root,
                         const boost::filesystem::path& canonical_web_root,
                         std::uint64_t max_buffered_request_body_size)
  : stream_(std::move(socket)),
    router_(router),
    websocket_router_(ws_router),
    web_root_path_(web_root),
    canonical_web_root_path_(canonical_web_root),
    max_buffered_request_body_size_(max_buffered_request_body_size)
{
  beast::error_code peer_ec;
  peer_endpoint_ = stream_.socket().remote_endpoint(peer_ec);
  if (peer_ec) peer_endpoint_.reset();
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
  res_ = {};
  ctx.reset();
  buffered_body_.clear();
  stream_completed_ = false;
  request_body_cancelled_ = false;
  request_parser_.emplace();
  request_parser_->header_limit(64 * 1024);
  request_parser_->body_limit((std::numeric_limits<std::uint64_t>::max)());
  http::async_read_header(stream_, buffer_, *request_parser_,
                          beast::bind_front_handler(&HttpSession::on_read_header, shared_from_this()));
}

void HttpSession::copy_request_head()
{
  const auto& source = request_parser_->get();
  req_.method(source.method());
  req_.target(source.target());
  req_.version(source.version());
  req_.keep_alive(source.keep_alive());
  for (const auto& field : source) req_.insert(field.name_string(), field.value());
}

void HttpSession::on_read_header(const beast::error_code& ec, std::size_t bytes_transferred)
{
  boost::ignore_unused(bytes_transferred);
  if (ec == http::error::end_of_stream) return do_close();
  if (ec) { spdlog::error("HttpSession header read error: {}", ec.message()); return do_close(); }
  copy_request_head();
  if (beast::websocket::is_upgrade(req_))
  {
    ctx = std::make_shared<HttpContext>(req_, res_, peer_endpoint_);
    return router_.async_run_pre_interceptors(*ctx, [self = shared_from_this()](InterceptorResult result)
    {
      net::post(self->stream_.get_executor(), [self, result]
      {
        if (result == InterceptorResult::Continue) self->handle_websocket_upgrade();
        else
        {
          self->router_.run_post_interceptors(*self->ctx);
          self->send_context_response();
        }
      });
    });
  }

  std::string path(req_.target());
  if (const auto query = path.find('?'); query != std::string::npos) path.resize(query);
  const bool stream_route = router_.is_stream_route(path, req_.method());
  if (!stream_route)
  {
    const auto content_length = request_parser_->content_length();
    if (content_length && *content_length > max_buffered_request_body_size_)
      return send_payload_too_large();
  }
  const auto expect = req_[http::field::expect];
  if (boost::beast::iequals(expect, "100-continue"))
  {
    auto response = std::make_shared<http::response<http::empty_body>>(http::status::continue_, req_.version());
    http::async_write(stream_, *response, [self = shared_from_this(), response, stream_route]
      (beast::error_code write_ec, std::size_t)
    {
      if (write_ec) return self->do_close();
      if (stream_route) self->handle_stream_request(); else self->read_buffered_body();
    });
    return;
  }
  if (stream_route) return handle_stream_request();
  read_buffered_body();
}

void HttpSession::read_buffered_body()
{
  if (request_parser_->is_done())
  {
    req_.body() = std::move(buffered_body_);
    return handle_request();
  }
  auto& body = request_parser_->get().body();
  body.data = buffered_body_chunk_.data();
  body.size = buffered_body_chunk_.size();
  http::async_read_some(stream_, buffer_, *request_parser_,
                        beast::bind_front_handler(&HttpSession::on_read_buffered_body, shared_from_this()));
}

void HttpSession::on_read_buffered_body(beast::error_code ec, std::size_t bytes_transferred)
{
  boost::ignore_unused(bytes_transferred);
  if (ec == http::error::need_buffer) ec = {};
  if (ec) { spdlog::error("HttpSession body read error: {}", ec.message()); return do_close(); }
  const auto produced = buffered_body_chunk_.size() - request_parser_->get().body().size;
  if (buffered_body_.size() > max_buffered_request_body_size_ ||
      produced > max_buffered_request_body_size_ - buffered_body_.size())
    return send_payload_too_large();
  buffered_body_.append(buffered_body_chunk_.data(), produced);
  read_buffered_body();
}

void HttpSession::send_payload_too_large()
{
  http::response<http::string_body> response{http::status::payload_too_large, req_.version()};
  response.keep_alive(false);
  response.body() = "request body exceeds buffered route limit";
  response.prepare_payload();
  send_response(std::move(response));
}

void HttpSession::handle_stream_request()
{
  ctx = std::make_shared<HttpContext>(req_, res_, peer_endpoint_);
  auto stream = std::make_shared<RequestStreamImpl>(shared_from_this());
  auto response_stream = std::make_shared<ResponseStreamImpl>(shared_from_this());
  std::weak_ptr<HttpSession> weak = shared_from_this();
  const auto complete = [weak]
  {
    if (auto self = weak.lock()) net::post(self->stream_.get_executor(), [self]
    {
      if (self->stream_completed_) return;
      self->stream_completed_ = true;
      self->router_.run_post_interceptors(*self->ctx);
      self->send_context_response();
    });
  };
  try
  {
    router_.async_run_pre_interceptors(*ctx,
      [self = shared_from_this(), stream = std::move(stream), response_stream = std::move(response_stream), complete]
      (InterceptorResult result) mutable
      {
        net::post(self->stream_.get_executor(),
          [self, result, stream = std::move(stream), response_stream = std::move(response_stream), complete]() mutable
          {
            if (result == InterceptorResult::Stop) return complete();
            try
            {
              if (!self->router_.dispatch_stream(*self->ctx, std::move(stream), std::move(response_stream), complete))
              {
                self->ctx->set_status(http::status::not_found);
                self->ctx->set_body("stream route not found");
                complete();
              }
            }
            catch (...) { self->router_.handle_exception(std::current_exception(), *self->ctx); complete(); }
          });
      });
  }
  catch (...) { router_.handle_exception(std::current_exception(), *ctx); complete(); }
}

void HttpSession::async_read_stream_body(net::mutable_buffer target, HttpRequestStream::ReadCallback callback)
{
  auto self = shared_from_this();
  net::post(stream_.get_executor(), [self, target, callback = std::move(callback)]() mutable
  {
    if (self->request_body_cancelled_) return callback(net::error::operation_aborted, 0, true);
    if (!self->request_parser_ || self->request_parser_->is_done()) return callback({}, 0, true);
    auto& body = self->request_parser_->get().body();
    body.data = target.data();
    body.size = target.size();
    http::async_read_some(self->stream_, self->buffer_, *self->request_parser_,
      [self, capacity = target.size(), callback = std::move(callback)](beast::error_code ec, std::size_t) mutable
      {
        if (ec == http::error::need_buffer) ec = {};
        const auto produced = capacity - self->request_parser_->get().body().size;
        callback(ec, produced, self->request_parser_->is_done());
      });
  });
}

void HttpSession::cancel_stream_body()
{
  auto self = shared_from_this();
  net::post(stream_.get_executor(), [self]
  {
    self->request_body_cancelled_ = true;
    // Cancel an outstanding body read without closing the socket. Subsequent
    // response writes remain valid; the connection is made non-persistent
    // because unread request bytes cannot be parsed as a next request.
    self->res_.keep_alive(false);
    self->stream_.cancel();
  });
}

void HttpSession::cancel_session()
{
  auto self = shared_from_this();
  net::post(stream_.get_executor(), [self]
  {
    beast::error_code ignored;
    self->stream_.cancel();
    self->stream_.socket().shutdown(tcp::socket::shutdown_both, ignored);
  });
}

void HttpSession::start_stream_response(HttpResponseStream::ResponseHead head, HttpResponseStream::Callback callback)
{
  auto self = shared_from_this();
  net::post(stream_.get_executor(), [self, head = std::move(head), callback = std::move(callback)]() mutable
  {
    self->disconnect_wait_cancelled_ = false;
    self->streaming_response_ = {};
    self->streaming_response_.result(head.result()); self->streaming_response_.version(head.version());
    self->streaming_response_.keep_alive(head.keep_alive());
    if (self->request_body_cancelled_) self->streaming_response_.keep_alive(false);
    for (const auto& field : head) self->streaming_response_.insert(field.name_string(), field.value());
    if (!self->streaming_response_.has_content_length() && !self->streaming_response_.chunked())
      self->streaming_response_.chunked(true);
    self->streaming_response_.body().more = true;
    self->streaming_response_serializer_.emplace(self->streaming_response_);
    http::async_write_header(self->stream_, *self->streaming_response_serializer_,
      [callback = std::move(callback)](beast::error_code ec, std::size_t) mutable
      { callback(ec); });
  });
}

void HttpSession::write_stream_response(net::const_buffer source, HttpResponseStream::Callback callback)
{
  auto self = shared_from_this();
  net::post(stream_.get_executor(), [self, source, callback = std::move(callback)]() mutable
  {
    if (!self->streaming_response_serializer_) return callback(net::error::operation_aborted);
    auto& body = self->streaming_response_.body();
    body.data = const_cast<void*>(source.data()); body.size = source.size(); body.more = true;
    http::async_write(self->stream_, *self->streaming_response_serializer_,
      [callback = std::move(callback)](beast::error_code ec, std::size_t) mutable
      { if (ec == http::error::need_buffer) ec = {}; callback(ec); });
  });
}

void HttpSession::finish_stream_response(HttpResponseStream::Callback callback)
{
  auto self = shared_from_this();
  net::post(stream_.get_executor(), [self, callback = std::move(callback)]() mutable
  {
    if (!self->streaming_response_serializer_) return callback(net::error::operation_aborted);
    if (self->streaming_response_serializer_->is_done())
    {
      callback({});
      const bool keep_alive = self->streaming_response_.keep_alive();
      self->streaming_response_serializer_.reset();
      if (keep_alive) self->do_read(); else self->do_close();
      return;
    }
    auto& body = self->streaming_response_.body(); body.data = nullptr; body.size = 0; body.more = false;
    http::async_write(self->stream_, *self->streaming_response_serializer_,
      [self, callback = std::move(callback)](beast::error_code ec, std::size_t) mutable
      {
        callback(ec);
        const bool keep_alive = self->streaming_response_.keep_alive();
        self->streaming_response_serializer_.reset();
        if (!ec && keep_alive) self->do_read(); else self->do_close();
      });
  });
}

void HttpSession::wait_stream_disconnect(HttpResponseStream::Callback callback)
{
  auto self = shared_from_this();
  net::post(stream_.get_executor(), [self, callback = std::move(callback)]() mutable
  {
    // A request body is still readable on this connection and must not be mistaken for a peer disconnect.
    if (self->disconnect_wait_cancelled_)
      return callback(net::error::operation_aborted);
    if ((self->request_parser_ && !self->request_parser_->is_done()) || self->disconnect_wait_active_) return;
    self->disconnect_wait_active_ = true;
    self->stream_.socket().async_receive(net::buffer(self->disconnect_probe_),
      net::bind_cancellation_slot(self->disconnect_wait_cancellation_.slot(),
        [self, callback = std::move(callback)](beast::error_code ec, const std::size_t bytes) mutable
        {
          self->disconnect_wait_active_ = false;
          if (!ec)
            ec = bytes == 0 ? net::error::eof : make_error_code(boost::system::errc::protocol_error);
          callback(ec);
        }));
  });
}

void HttpSession::cancel_stream_disconnect_wait()
{
  auto self = shared_from_this();
  net::dispatch(stream_.get_executor(), [self]
  {
    self->disconnect_wait_cancelled_ = true;
    if (self->disconnect_wait_active_)
      self->disconnect_wait_cancellation_.emit(net::cancellation_type::terminal);
  });
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

  ctx = std::make_shared<HttpContext>(req_, res_, peer_endpoint_);

  try
  {
    router_.async_run_pre_interceptors(*ctx, [self = shared_from_this()](InterceptorResult result)
    { net::post(self->stream_.get_executor(), [self, result] { self->dispatch_request_after_interceptors(result); }); });
  }
  catch (...)
  {
    router_.handle_exception(std::current_exception(), *ctx);
    try
    {
      router_.run_post_interceptors(*ctx);
    }
    catch (...)
    {
      router_.handle_exception(std::current_exception(), *ctx);
    }
    send_context_response();
  }
}

void HttpSession::dispatch_request_after_interceptors(InterceptorResult result)
{
  bool post_interceptors_started = false;
  try
  {
    if (result == InterceptorResult::Stop)
    {
      post_interceptors_started = true;
      router_.run_post_interceptors(*ctx);
      return send_context_response();
    }

    // Asynchronous routes take precedence when registered for this method.
    if (router_.dispatch_async(*ctx, [self = shared_from_this()]
      { net::post(self->stream_.get_executor(), [self]
        {
          try { self->router_.run_post_interceptors(*self->ctx); self->send_context_response(); }
          catch (...) { self->router_.handle_exception(std::current_exception(), *self->ctx); self->send_context_response(); }
        }); })) return;

    bool static_file_served = false;
    router_.dispatch(*ctx, [this, &static_file_served]
    {
      if (req_.method() == http::verb::get || req_.method() == http::verb::head)
        static_file_served = do_serve_static_file();
      return static_file_served;
    });
    if (static_file_served) return;
    post_interceptors_started = true;
    router_.run_post_interceptors(*ctx);
    send_context_response();
  }
  catch (...)
  {
    router_.handle_exception(std::current_exception(), *ctx);
    if (!post_interceptors_started)
    {
      try
      {
        post_interceptors_started = true;
        router_.run_post_interceptors(*ctx);
      }
      catch (...)
      {
        router_.handle_exception(std::current_exception(), *ctx);
      }
    }
    send_context_response();
  }
}

void HttpSession::send_context_response()
{
  if (request_body_cancelled_) res_.keep_alive(false);
  if (req_.method() == http::verb::head)
  {
    http::response<http::empty_body> head{res_.result(), res_.version()};
    head.keep_alive(res_.keep_alive());
    for (const auto& field : res_) head.insert(field.name_string(), field.value());
    head.erase(http::field::transfer_encoding);
    if (!head.has_content_length()) head.content_length(res_.body().size());
    return send_response(std::move(head));
  }
  if (res_.chunked()) send_chunked_response();
  else
  {
    if (!res_.has_content_length()) res_.prepare_payload();
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
  spdlog::debug("HttpSession closing connection");
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
