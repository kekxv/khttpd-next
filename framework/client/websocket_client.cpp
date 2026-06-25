#include "websocket_client.hpp"
#include <iostream>
#include <mutex>
#include <boost/url.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>

#include "io_context_pool.hpp"

namespace khttpd::framework::client
{
  struct WebsocketClient::State
  {
    std::mutex mutex;
    bool alive = true;
    bool close_notified = false;
    MessageHandler on_message;
    ErrorHandler on_error;
    CloseHandler on_close;
  };

  // ==========================================
  // Internal Session Abstraction
  // ==========================================
  struct WebsocketSessionImpl : public std::enable_shared_from_this<WebsocketSessionImpl>
  {
    std::weak_ptr<WebsocketClient::State> state_;
    std::string host_;
    beast::flat_buffer buffer_;
    std::deque<std::string> write_queue_; // 写队列
    bool is_writing_ = false;
    bool closing_ = false;
    bool close_started_ = false;
    bool connect_notified_ = false;

    explicit WebsocketSessionImpl(std::shared_ptr<WebsocketClient::State> state) : state_(std::move(state))
    {
    }

    virtual ~WebsocketSessionImpl() = default;

    virtual void run(const std::string& host, const std::string& port, const std::string& target,
                     const std::map<std::string, std::string>& headers, WebsocketClient::ConnectCallback cb) = 0;
    virtual void close() = 0;

    // 核心发送逻辑：入队
    void queue_write(std::string message)
    {
      net::post(get_executor(), beast::bind_front_handler(
                  &WebsocketSessionImpl::on_queue_write, shared_from_this(), std::move(message)));
    }

  protected:
    virtual net::any_io_executor get_executor() = 0;
    virtual void do_write_from_queue() = 0;

    void notify_message(const std::string& message)
    {
      auto state = state_.lock();
      if (!state) return;
      WebsocketClient::MessageHandler handler;
      {
        std::lock_guard<std::mutex> lock{state->mutex};
        if (!state->alive) return;
        handler = state->on_message;
      }
      if (handler) handler(message);
    }

    void notify_error(beast::error_code ec)
    {
      auto state = state_.lock();
      if (!state) return;
      WebsocketClient::ErrorHandler handler;
      {
        std::lock_guard<std::mutex> lock{state->mutex};
        if (!state->alive) return;
        handler = state->on_error;
      }
      if (handler) handler(ec);
    }

    void notify_close()
    {
      auto state = state_.lock();
      if (!state) return;
      WebsocketClient::CloseHandler handler;
      {
        std::lock_guard<std::mutex> lock{state->mutex};
        if (!state->alive || state->close_notified) return;
        state->close_notified = true;
        handler = state->on_close;
      }
      if (handler) handler();
    }

    void notify_connect(WebsocketClient::ConnectCallback& callback, beast::error_code ec)
    {
      if (connect_notified_ || closing_) return;
      connect_notified_ = true;

      auto state = state_.lock();
      if (!state) return;
      {
        std::lock_guard<std::mutex> lock{state->mutex};
        if (!state->alive) return;
      }
      if (callback) callback(ec);
    }

    void on_queue_write(std::string message)
    {
      if (closing_) return;
      write_queue_.push_back(std::move(message));
      if (!is_writing_)
      {
        is_writing_ = true;
        do_write_from_queue();
      }
    }

    // 通用的读循环处理
    void process_read_result(beast::error_code ec, std::size_t bytes)
    {
      boost::ignore_unused(bytes);
      if (ec)
      {
        // 修改：增加 operation_aborted 到关闭判定条件中
        // 当 async_read 被取消（例如正在关闭时），也应视为连接断开
        if (ec == websocket::error::closed ||
          ec == net::error::eof ||
          ec == ssl::error::stream_truncated ||
          ec == boost::asio::error::connection_reset ||
          ec == boost::asio::error::operation_aborted)
        {
          notify_close();
        }
        else
        {
          notify_error(ec);
        }
        return;
      }

      notify_message(beast::buffers_to_string(buffer_.data()));
      buffer_.consume(buffer_.size());
    }

    // 通用的写完成处理
    void process_write_result(beast::error_code ec)
    {
      if (ec)
      {
        is_writing_ = false; // Stop writing on error
        if (ec == websocket::error::closed || ec == net::error::operation_aborted)
        {
          notify_close();
        }
        else
        {
          notify_error(ec);
        }
        return;
      }

      write_queue_.pop_front();

      if (!write_queue_.empty())
      {
        do_write_from_queue();
      }
      else
      {
        is_writing_ = false;
      }
    }
  };

  // ==========================================
  // Plain TCP Session (ws://)
  // ==========================================
  class PlainWebsocketSession : public WebsocketSessionImpl
  {
    websocket::stream<beast::tcp_stream> ws_;
    tcp::resolver resolver_;
    WebsocketClient::ConnectCallback connect_cb_;

  public:
    PlainWebsocketSession(net::io_context& ioc, std::shared_ptr<WebsocketClient::State> state)
      : WebsocketSessionImpl(std::move(state)), ws_(net::make_strand(ioc)), resolver_(ioc)
    {
    }

    net::any_io_executor get_executor() override { return ws_.get_executor(); }

    void run(const std::string& host, const std::string& port, const std::string& target,
             const std::map<std::string, std::string>& headers, WebsocketClient::ConnectCallback cb) override
    {
      host_ = host;
      connect_cb_ = std::move(cb);

      resolver_.async_resolve(host, port, beast::bind_front_handler(&PlainWebsocketSession::on_resolve,
                                                                    std::static_pointer_cast<PlainWebsocketSession>(
                                                                      shared_from_this()), target, headers));
    }

    void close() override
    {
      net::post(ws_.get_executor(), [self = std::static_pointer_cast<PlainWebsocketSession>(shared_from_this())]()
      {
        self->closing_ = true;
        self->write_queue_.clear();
        beast::error_code ignored;
        self->resolver_.cancel();
        beast::get_lowest_layer(self->ws_).cancel();

        if (self->ws_.is_open() && !self->close_started_)
        {
          self->close_started_ = true;
          self->ws_.async_close(websocket::close_code::normal, [self](beast::error_code)
          {
            /* ignore close error */
            self->notify_close();
          });
          return;
        }

        beast::get_lowest_layer(self->ws_).socket().shutdown(tcp::socket::shutdown_both, ignored);
        beast::get_lowest_layer(self->ws_).socket().close(ignored);
      });
    }

  protected:
    void do_write_from_queue() override
    {
      ws_.async_write(net::buffer(write_queue_.front()),
                      beast::bind_front_handler(&PlainWebsocketSession::on_write,
                                                std::static_pointer_cast<PlainWebsocketSession>(shared_from_this())));
    }

  private:
    void on_resolve(std::string target, std::map<std::string, std::string> headers, beast::error_code ec,
                    tcp::resolver::results_type results)
    {
      if (closing_) return;
      if (ec) return fail(ec);
      beast::get_lowest_layer(ws_).async_connect(results, beast::bind_front_handler(
                                                   &PlainWebsocketSession::on_connect,
                                                   std::static_pointer_cast<PlainWebsocketSession>(shared_from_this()),
                                                   target, headers));
    }

    void on_connect(std::string target, std::map<std::string, std::string> headers, beast::error_code ec,
                    tcp::resolver::results_type::endpoint_type)
    {
      if (closing_) return;
      if (ec) return fail(ec);

      ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));

      // Set Headers
      ws_.set_option(websocket::stream_base::decorator([headers](websocket::request_type& req)
      {
        req.set(beast::http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        for (const auto& h : headers) req.set(h.first, h.second);
      }));

      ws_.async_handshake(host_, target,
                          beast::bind_front_handler(&PlainWebsocketSession::on_handshake,
                                                    std::static_pointer_cast<PlainWebsocketSession>(
                                                      shared_from_this())));
    }

    void on_handshake(beast::error_code ec)
    {
      if (closing_) return;
      if (ec) return fail(ec);
      notify_connect(connect_cb_, ec);
      do_read();
    }

    void do_read()
    {
      ws_.async_read(buffer_, beast::bind_front_handler(&PlainWebsocketSession::on_read,
                                                        std::static_pointer_cast<PlainWebsocketSession>(
                                                          shared_from_this())));
    }

    void on_read(beast::error_code ec, std::size_t bytes)
    {
      process_read_result(ec, bytes);
      if (!ec) do_read();
    }

    void on_write(beast::error_code ec, std::size_t)
    {
      process_write_result(ec);
    }

    void fail(beast::error_code ec)
    {
      notify_connect(connect_cb_, ec);
    }
  };

  // ==========================================
  // SSL Session (wss://)
  // ==========================================
  class SslWebsocketSession : public WebsocketSessionImpl
  {
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws_;
    tcp::resolver resolver_;
    WebsocketClient::ConnectCallback connect_cb_;

  public:
    SslWebsocketSession(net::io_context& ioc, ssl::context& ctx, std::shared_ptr<WebsocketClient::State> state)
      : WebsocketSessionImpl(std::move(state)), ws_(net::make_strand(ioc), ctx), resolver_(ioc)
    {
    }

    net::any_io_executor get_executor() override { return ws_.get_executor(); }

    void run(const std::string& host, const std::string& port, const std::string& target,
             const std::map<std::string, std::string>& headers, WebsocketClient::ConnectCallback cb) override
    {
      host_ = host;
      connect_cb_ = std::move(cb);

      if (!SSL_set_tlsext_host_name(ws_.next_layer().native_handle(), host.c_str()))
      {
        return fail(beast::error_code(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()));
      }
      ws_.next_layer().set_verify_callback(ssl::host_name_verification(host));

      resolver_.async_resolve(host, port, beast::bind_front_handler(&SslWebsocketSession::on_resolve,
                                                                    std::static_pointer_cast<SslWebsocketSession>(
                                                                      shared_from_this()), target, headers));
    }

    void close() override
    {
      net::post(ws_.get_executor(), [self = std::static_pointer_cast<SslWebsocketSession>(shared_from_this())]()
      {
        self->closing_ = true;
        self->write_queue_.clear();
        beast::error_code ignored;
        self->resolver_.cancel();
        beast::get_lowest_layer(self->ws_).cancel();

        if (self->ws_.is_open() && !self->close_started_)
        {
          self->close_started_ = true;
          self->ws_.async_close(websocket::close_code::normal, [self](beast::error_code)
          {
            self->notify_close();
          });
          return;
        }

        beast::get_lowest_layer(self->ws_).socket().shutdown(tcp::socket::shutdown_both, ignored);
        beast::get_lowest_layer(self->ws_).socket().close(ignored);
      });
    }

  protected:
    void do_write_from_queue() override
    {
      ws_.async_write(net::buffer(write_queue_.front()),
                      beast::bind_front_handler(&SslWebsocketSession::on_write,
                                                std::static_pointer_cast<SslWebsocketSession>(shared_from_this())));
    }

  private:
    void on_resolve(std::string target, std::map<std::string, std::string> headers, beast::error_code ec,
                    tcp::resolver::results_type results)
    {
      if (closing_) return;
      if (ec) return fail(ec);
      beast::get_lowest_layer(ws_).async_connect(results, beast::bind_front_handler(
                                                   &SslWebsocketSession::on_connect,
                                                   std::static_pointer_cast<SslWebsocketSession>(shared_from_this()),
                                                   target, headers));
    }

    void on_connect(std::string target, std::map<std::string, std::string> headers, beast::error_code ec,
                    tcp::resolver::results_type::endpoint_type)
    {
      if (closing_) return;
      if (ec) return fail(ec);
      ws_.next_layer().async_handshake(ssl::stream_base::client,
                                       beast::bind_front_handler(&SslWebsocketSession::on_ssl_handshake,
                                                                 std::static_pointer_cast<SslWebsocketSession>(
                                                                   shared_from_this()), target, headers));
    }

    void on_ssl_handshake(std::string target, std::map<std::string, std::string> headers, beast::error_code ec)
    {
      if (closing_) return;
      if (ec) return fail(ec);

      ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
      ws_.set_option(websocket::stream_base::decorator([headers](websocket::request_type& req)
      {
        req.set(beast::http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        for (const auto& h : headers) req.set(h.first, h.second);
      }));

      ws_.async_handshake(host_, target,
                          beast::bind_front_handler(&SslWebsocketSession::on_handshake,
                                                    std::static_pointer_cast<SslWebsocketSession>(shared_from_this())));
    }

    void on_handshake(beast::error_code ec)
    {
      if (closing_) return;
      if (ec) return fail(ec);
      notify_connect(connect_cb_, ec);
      do_read();
    }

    void do_read()
    {
      ws_.async_read(buffer_, beast::bind_front_handler(&SslWebsocketSession::on_read,
                                                        std::static_pointer_cast<SslWebsocketSession>(
                                                          shared_from_this())));
    }

    void on_read(beast::error_code ec, std::size_t bytes)
    {
      process_read_result(ec, bytes);
      if (!ec) do_read();
    }

    void on_write(beast::error_code ec, std::size_t)
    {
      process_write_result(ec);
    }

    void fail(beast::error_code ec)
    {
      notify_connect(connect_cb_, ec);
    }
  };

  // ==========================================
  // WebsocketClient Implementation
  // ==========================================

  WebsocketClient::WebsocketClient()
      : ioc_(IoContextPool::instance().get_io_context()),
        state_(std::make_shared<State>())
  {
    own_ssl_ctx_ = std::make_shared<ssl::context>(ssl::context::tls_client);
    own_ssl_ctx_->set_default_verify_paths();
    own_ssl_ctx_->set_verify_mode(ssl::verify_peer);
    ssl_ctx_ptr_ = own_ssl_ctx_.get();
  }

  WebsocketClient::WebsocketClient(net::io_context& ioc)
    : ioc_(ioc),
      state_(std::make_shared<State>())
  {
    // Default SSL Context
    own_ssl_ctx_ = std::make_shared<ssl::context>(ssl::context::tls_client);
    own_ssl_ctx_->set_default_verify_paths();
    own_ssl_ctx_->set_verify_mode(ssl::verify_peer);
    ssl_ctx_ptr_ = own_ssl_ctx_.get();
  }

  WebsocketClient::WebsocketClient(net::io_context& ioc, ssl::context& ssl_ctx)
    : ioc_(ioc), ssl_ctx_ptr_(&ssl_ctx), state_(std::make_shared<State>())
  {
  }

  WebsocketClient::~WebsocketClient()
  {
    {
      std::lock_guard<std::mutex> lock{state_->mutex};
      state_->alive = false;
      state_->on_message = nullptr;
      state_->on_error = nullptr;
      state_->on_close = nullptr;
    }
    close();
  }

  void WebsocketClient::set_header(const std::string& key, const std::string& value)
  {
    headers_[key] = value;
  }

  void WebsocketClient::connect(const std::string& url, ConnectCallback callback)
  {
    auto url_result = boost::urls::parse_uri(url);
    if (!url_result.has_value())
    {
      if (callback) callback(beast::error_code(beast::http::error::bad_target));
      return;
    }
    auto u = url_result.value();
    std::string host = u.host();
    std::string scheme = u.scheme();
    std::string port = u.port();
    std::string target = u.encoded_path().data();
    if (target.empty()) target = "/";

    if (port.empty()) port = (scheme == "wss") ? "443" : "80";

    if (scheme == "wss")
    {
      if (!ssl_ctx_ptr_)
      {
        if (callback) callback(beast::error_code(beast::errc::operation_not_supported, beast::system_category()));
        return;
      }
      {
        std::lock_guard<std::mutex> lock{state_->mutex};
        state_->close_notified = false;
      }
      auto s = std::make_shared<SslWebsocketSession>(ioc_, *ssl_ctx_ptr_, state_);
      session_ = s;
      s->run(host, port, target, headers_, std::move(callback));
    }
    else
    {
      {
        std::lock_guard<std::mutex> lock{state_->mutex};
        state_->close_notified = false;
      }
      auto s = std::make_shared<PlainWebsocketSession>(ioc_, state_);
      session_ = s;
      s->run(host, port, target, headers_, std::move(callback));
    }
  }

  void WebsocketClient::send(const std::string& message)
  {
    if (session_)
    {
      session_->queue_write(message);
    }
  }

  void WebsocketClient::close()
  {
    if (session_)
    {
      session_->close();
      // session_ = nullptr; // keep alive for handlers to finish
    }
  }

  void WebsocketClient::set_on_message(MessageHandler handler)
  {
    std::lock_guard<std::mutex> lock{state_->mutex};
    state_->on_message = std::move(handler);
  }

  void WebsocketClient::set_on_error(ErrorHandler handler)
  {
    std::lock_guard<std::mutex> lock{state_->mutex};
    state_->on_error = std::move(handler);
  }

  void WebsocketClient::set_on_close(CloseHandler handler)
  {
    std::lock_guard<std::mutex> lock{state_->mutex};
    state_->on_close = std::move(handler);
  }
}
