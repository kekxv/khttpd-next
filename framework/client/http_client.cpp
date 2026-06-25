#include "http_client.hpp"
#include <boost/asio/connect.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <atomic>
#include <iostream>
#include "io_context_pool.hpp"

namespace khttpd::framework::client
{
  std::string replace_all(std::string str, const std::string& from, const std::string& to)
  {
    if (from.empty()) return str;
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos)
    {
      str.replace(start_pos, from.length(), to);
      start_pos += to.length();
    }
    return str;
  }

  // ==========================================
  // Abstract Session to handle common logic
  // ==========================================
  class Session : public std::enable_shared_from_this<Session>
  {
  protected:
    HttpClient::ResponseCallback callback_;
    http::request<http::string_body> req_;
    http::response<http::string_body> res_;
    beast::flat_buffer buffer_;
    std::chrono::seconds timeout_;
    std::atomic<bool> completed_{false};

  public:
    Session(HttpClient::ResponseCallback callback, std::chrono::seconds timeout)
      : callback_(std::move(callback)), timeout_(timeout)
    {
    }

    virtual ~Session() = default;
    virtual void run(const std::string& host, const std::string& port, http::request<http::string_body> req) = 0;
    virtual void cancel() = 0;

  protected:
    void complete(beast::error_code ec, http::response<http::string_body> res)
    {
      if (!completed_.exchange(true) && callback_)
      {
        callback_(ec, std::move(res));
      }
    }

    void on_fail(beast::error_code ec, const char* what)
    {
      // Log if needed: std::cerr << what << ": " << ec.message() << "\n";
      boost::ignore_unused(what);
      complete(ec, {});
    }
  };

  // ==========================================
  // Plain HTTP Session
  // ==========================================
  class HttpSession : public Session
  {
    beast::tcp_stream stream_;
    tcp::resolver resolver_;

    // Helper: Downcast shared_from_this to avoid template deduction errors
    std::shared_ptr<HttpSession> get_shared()
    {
      return std::static_pointer_cast<HttpSession>(shared_from_this());
    }

  public:
    HttpSession(net::io_context& ioc, HttpClient::ResponseCallback cb, std::chrono::seconds timeout)
      : Session(std::move(cb), timeout), stream_(net::make_strand(ioc)), resolver_(stream_.get_executor())
    {
    }

    void run(const std::string& host, const std::string& port, http::request<http::string_body> req) override
    {
      req_ = std::move(req);
      stream_.expires_after(timeout_);
      resolver_.async_resolve(host, port,
                              beast::bind_front_handler(&HttpSession::on_resolve, get_shared()));
    }

    void cancel() override
    {
      net::post(stream_.get_executor(), [self = get_shared()]()
      {
        beast::error_code ignored;
        self->resolver_.cancel();
        self->stream_.cancel();
        self->stream_.socket().shutdown(tcp::socket::shutdown_both, ignored);
        self->stream_.socket().close(ignored);
      });
    }

    void on_resolve(beast::error_code ec, tcp::resolver::results_type results)
    {
      if (ec) return on_fail(ec, "resolve");
      stream_.expires_after(timeout_);
      stream_.async_connect(results,
                            beast::bind_front_handler(&HttpSession::on_connect, get_shared()));
    }

    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type)
    {
      if (ec) return on_fail(ec, "connect");
      stream_.expires_after(timeout_);
      http::async_write(stream_, req_,
                        beast::bind_front_handler(&HttpSession::on_write, get_shared()));
    }

    void on_write(beast::error_code ec, std::size_t bytes_transferred)
    {
      boost::ignore_unused(bytes_transferred);
      if (ec) return on_fail(ec, "write");

      http::async_read(stream_, buffer_, res_,
                       beast::bind_front_handler(&HttpSession::on_read, get_shared()));
    }

    void on_read(beast::error_code ec, std::size_t bytes_transferred)
    {
      boost::ignore_unused(bytes_transferred);
      if (ec) return on_fail(ec, "read");

      stream_.socket().shutdown(tcp::socket::shutdown_both, ec);
      complete(ec, std::move(res_));
    }
  };

  // ==========================================
  // HTTPS Session
  // ==========================================
  class HttpsSession : public Session
  {
    beast::ssl_stream<beast::tcp_stream> stream_;
    tcp::resolver resolver_;

    std::shared_ptr<HttpsSession> get_shared()
    {
      return std::static_pointer_cast<HttpsSession>(shared_from_this());
    }

  public:
    HttpsSession(net::io_context& ioc, ssl::context& ctx, HttpClient::ResponseCallback cb, std::chrono::seconds timeout)
      : Session(std::move(cb), timeout), stream_(net::make_strand(ioc), ctx), resolver_(stream_.get_executor())
    {
    }

    void run(const std::string& host, const std::string& port, http::request<http::string_body> req) override
    {
      req_ = std::move(req);
      if (!SSL_set_tlsext_host_name(stream_.native_handle(), host.c_str()))
      {
        beast::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
        return on_fail(ec, "ssl_setup");
      }
      stream_.set_verify_callback(ssl::host_name_verification(host));

      stream_.next_layer().expires_after(timeout_);
      resolver_.async_resolve(host, port,
                              beast::bind_front_handler(&HttpsSession::on_resolve, get_shared()));
    }

    void cancel() override
    {
      net::post(stream_.get_executor(), [self = get_shared()]()
      {
        beast::error_code ignored;
        self->resolver_.cancel();
        beast::get_lowest_layer(self->stream_).cancel();
        beast::get_lowest_layer(self->stream_).socket().shutdown(tcp::socket::shutdown_both, ignored);
        beast::get_lowest_layer(self->stream_).socket().close(ignored);
      });
    }

    void on_resolve(beast::error_code ec, tcp::resolver::results_type results)
    {
      if (ec) return on_fail(ec, "resolve");
      stream_.next_layer().expires_after(timeout_);
      beast::get_lowest_layer(stream_).async_connect(results,
                                                     beast::bind_front_handler(
                                                       &HttpsSession::on_connect, get_shared()));
    }

    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type)
    {
      if (ec) return on_fail(ec, "connect");
      stream_.next_layer().expires_after(timeout_);
      stream_.async_handshake(ssl::stream_base::client,
                              beast::bind_front_handler(&HttpsSession::on_handshake, get_shared()));
    }

    void on_handshake(beast::error_code ec)
    {
      if (ec) return on_fail(ec, "handshake");
      stream_.next_layer().expires_after(timeout_);
      http::async_write(stream_, req_,
                        beast::bind_front_handler(&HttpsSession::on_write, get_shared()));
    }

    void on_write(beast::error_code ec, std::size_t bytes_transferred)
    {
      boost::ignore_unused(bytes_transferred);
      if (ec) return on_fail(ec, "write");
      http::async_read(stream_, buffer_, res_,
                       beast::bind_front_handler(&HttpsSession::on_read, get_shared()));
    }

    void on_read(beast::error_code ec, std::size_t bytes_transferred)
    {
      boost::ignore_unused(bytes_transferred);
      if (ec)
      {
        if ((ec == ssl::error::stream_truncated || ec == net::error::eof) && res_.result_int() != 0)
        {
          complete({}, std::move(res_));
          return;
        }
        return on_fail(ec, "read");
      }

      stream_.async_shutdown(beast::bind_front_handler(&HttpsSession::on_shutdown, get_shared()));
    }

    void on_shutdown(beast::error_code ec)
    {
      if (ec == net::error::eof || ec == ssl::error::stream_truncated)
        ec = {};
      complete(ec, std::move(res_));
    }
  };

  // 1. 傻瓜式：全局 IO + 默认 SSL
  HttpClient::HttpClient()
    : ioc_(IoContextPool::instance().get_io_context()) // 从单例获取
  {
    // 同样的默认 SSL 初始化逻辑
    own_ssl_ctx_ = std::make_shared<ssl::context>(ssl::context::tls_client);
    own_ssl_ctx_->set_default_verify_paths();
    own_ssl_ctx_->set_verify_mode(ssl::verify_peer);
    ssl_ctx_ptr_ = own_ssl_ctx_.get();
  }

  // 2. 全局 IO + 自定义 SSL
  HttpClient::HttpClient(ssl::context& ssl_ctx)
    : ioc_(IoContextPool::instance().get_io_context())
      , ssl_ctx_ptr_(&ssl_ctx)
  {
  }

  // 3. 自定义 IO + 默认 SSL (原逻辑)
  HttpClient::HttpClient(net::io_context& ioc)
    : ioc_(ioc)
  {
    own_ssl_ctx_ = std::make_shared<ssl::context>(ssl::context::tls_client);
    own_ssl_ctx_->set_default_verify_paths();
    own_ssl_ctx_->set_verify_mode(ssl::verify_peer);
    ssl_ctx_ptr_ = own_ssl_ctx_.get();
  }

  // 4. 全自定义
  HttpClient::HttpClient(net::io_context& ioc, ssl::context& ssl_ctx)
    : ioc_(ioc)
      , ssl_ctx_ptr_(&ssl_ctx)
  {
  }

  void HttpClient::set_base_url(const std::string& url)
  {
    host_pool_.reset(); // Clear pool, revert to single host
    auto result = boost::urls::parse_uri(url);
    if (result.has_value())
    {
      base_url_ = result.value();
    }
    else
    {
      // Fallback for missing scheme
      if (url.find("http") != 0)
      {
        auto res2 = boost::urls::parse_uri("http://" + url);
        if (res2.has_value()) base_url_ = res2.value();
      }
    }
  }

  void HttpClient::set_base_url_pool(const std::vector<HostEntry>& hosts)
  {
    if (hosts.empty())
    {
      host_pool_.reset();
      return;
    }
    if (hosts.size() == 1)
    {
      // Single host: fall back to set_base_url for simplicity
      set_base_url(hosts[0].url);
      host_pool_.reset();
      return;
    }
    host_pool_ = std::make_unique<HostPool>(hosts);
    base_url_.reset(); // Clear single host URL
  }

  void HttpClient::set_default_header(const std::string& key, const std::string& value)
  {
    default_headers_[key] = value;
  }

  void HttpClient::set_bearer_token(const std::string& token)
  {
    set_default_header("Authorization", "Bearer " + token);
  }

  void HttpClient::set_timeout(std::chrono::seconds seconds)
  {
    timeout_ = seconds;
  }

  HttpClient::UrlParts HttpClient::parse_target(const std::string& path_in,
                                                const std::map<std::string, std::string>& query)
  {
    boost::urls::url u;

    // Use host pool if available (multi-host), otherwise use single base_url_
    if (host_pool_)
    {
      const std::string& host_url = host_pool_->pick();
      auto pool_res = boost::urls::parse_uri(host_url);
      if (pool_res.has_value())
      {
        u = pool_res.value();
      }
      else
      {
        auto fallback = boost::urls::parse_uri("http://" + host_url);
        if (fallback.has_value()) u = fallback.value();
      }
      if (!path_in.empty())
      {
        if (path_in.front() != '/') u.set_path(u.path() + "/" + path_in);
        else u.set_path(path_in);
      }
    }
    else if (base_url_.has_value())
    {
      u = base_url_.value();
      if (!path_in.empty())
      {
        if (path_in.front() != '/') u.set_path(u.path() + "/" + path_in);
        else u.set_path(path_in);
      }
    }

    auto parse_res = boost::urls::parse_uri(path_in);
    if (parse_res.has_value())
    {
      u = parse_res.value();
    }

    for (const auto& [k, v] : query)
    {
      u.params().append({k, v});
    }

    UrlParts parts;
    parts.scheme = u.scheme();
    parts.host = u.host();
    parts.port = u.port();
    parts.target = u.encoded_target();

    if (parts.scheme.empty()) parts.scheme = "http";
    if (parts.target.empty()) parts.target = "/";
    if (parts.port.empty()) parts.port = (parts.scheme == "https") ? "443" : "80";

    return parts;
  }

  void HttpClient::request(http::verb method,
                           std::string path,
                           const std::map<std::string, std::string>& query_params,
                           const std::string& body,
                           const std::map<std::string, std::string>& headers,
                           ResponseCallback callback)
  {
    try
    {
      auto parts = parse_target(path, query_params);

      http::request<http::string_body> req{method, parts.target, 11};
      req.set(http::field::host, parts.host);
      req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

      for (const auto& h : default_headers_) req.set(h.first, h.second);
      for (const auto& h : headers) req.set(h.first, h.second);

      if (!body.empty())
      {
        req.body() = body;
        req.prepare_payload();
      }

      std::shared_ptr<Session> session;
      if (parts.scheme == "https")
      {
        if (!ssl_ctx_ptr_)
        {
          if (callback) callback(beast::error_code(beast::errc::operation_not_supported, beast::system_category()), {});
          return;
        }
        session = std::make_shared<HttpsSession>(ioc_, *ssl_ctx_ptr_, std::move(callback), timeout_);
      }
      else
      {
        session = std::make_shared<HttpSession>(ioc_, std::move(callback), timeout_);
      }
      session->run(parts.host, parts.port, std::move(req));
    }
    catch (const std::exception& e)
    {
      if (callback) callback(beast::error_code(beast::errc::invalid_argument, beast::system_category()), {});
    }
  }

  http::response<http::string_body> HttpClient::request_sync(
    http::verb method,
    std::string path,
    const std::map<std::string, std::string>& query_params,
    const std::string& body,
    const std::map<std::string, std::string>& headers)
  {
    auto p = std::make_shared<std::promise<std::pair<beast::error_code, http::response<http::string_body>>>>();
    auto completed = std::make_shared<std::atomic<bool>>(false);
    auto f = p->get_future();
    std::shared_ptr<Session> session;

    try
    {
      auto parts = parse_target(path, query_params);

      http::request<http::string_body> req{method, parts.target, 11};
      req.set(http::field::host, parts.host);
      req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

      for (const auto& h : default_headers_) req.set(h.first, h.second);
      for (const auto& h : headers) req.set(h.first, h.second);

      if (!body.empty())
      {
        req.body() = body;
        req.prepare_payload();
      }

      auto callback = [p, completed](beast::error_code ec, http::response<http::string_body> res)
      {
        if (!completed->exchange(true))
        {
          p->set_value({ec, std::move(res)});
        }
      };

      if (parts.scheme == "https")
      {
        if (!ssl_ctx_ptr_)
        {
          throw boost::system::system_error(
            beast::error_code(beast::errc::operation_not_supported, beast::system_category()));
        }
        session = std::make_shared<HttpsSession>(ioc_, *ssl_ctx_ptr_, std::move(callback), timeout_);
      }
      else
      {
        session = std::make_shared<HttpSession>(ioc_, std::move(callback), timeout_);
      }
      session->run(parts.host, parts.port, std::move(req));
    }
    catch (const boost::system::system_error&)
    {
      throw;
    }
    catch (const std::exception&)
    {
      throw boost::system::system_error(
        beast::error_code(beast::errc::invalid_argument, beast::system_category()));
    }

    if (f.wait_for(timeout_ + std::chrono::seconds(1)) != std::future_status::ready)
    {
      completed->store(true);
      if (session) session->cancel();
      throw boost::system::system_error(beast::error_code(beast::errc::timed_out, beast::system_category()));
    }
    auto result = f.get();

    if (result.first)
    {
      throw boost::system::system_error(result.first);
    }
    return result.second;
  }
}
