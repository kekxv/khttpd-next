#include "http_proxy_session.hpp"
#include <spdlog/spdlog.h>
#include "io_context_pool.hpp"

namespace khttpd::framework::client
{
  namespace net = boost::asio;
  namespace beast = boost::beast;

  namespace
  {
    template <class Message>
    void filter_hop_by_hop(Message& message)
    {
      const bool chunked = message.chunked();
      if (const auto connection = message[beast::http::field::connection]; !connection.empty())
      {
        std::string tokens(connection);
        std::size_t begin = 0;
        while (begin < tokens.size())
        {
          const auto end = tokens.find(',', begin);
          auto token = tokens.substr(begin, end == std::string::npos ? end : end - begin);
          const auto first = token.find_first_not_of(" \t");
          if (first != std::string::npos)
          {
            token = token.substr(first, token.find_last_not_of(" \t") - first + 1);
            message.erase(token);
          }
          if (end == std::string::npos) break;
          begin = end + 1;
        }
      }
      message.erase(beast::http::field::connection);
      message.erase(beast::http::field::keep_alive);
      message.erase(beast::http::field::proxy_authenticate);
      message.erase(beast::http::field::proxy_authorization);
      message.erase(beast::http::field::te);
      message.erase(beast::http::field::trailer);
      message.erase(beast::http::field::transfer_encoding);
      message.erase(beast::http::field::upgrade);
      message.erase("Proxy-Connection");
      if (chunked) message.chunked(true);
    }
  }

  HttpProxySession::HttpProxySession(std::shared_ptr<HttpRequestStream> inbound,
                                     std::shared_ptr<HttpResponseStream> downstream, std::size_t buffer_size)
    : HttpProxySession(IoContextPool::instance().get_io_context(), std::move(inbound),
                       std::move(downstream), buffer_size) {}

  HttpProxySession::HttpProxySession(std::shared_ptr<HttpRequestStream> inbound,
                                     std::shared_ptr<HttpResponseStream> downstream,
                                     net::ssl::context& ssl_context, std::size_t buffer_size)
    : HttpProxySession(IoContextPool::instance().get_io_context(), std::move(inbound),
                       std::move(downstream), ssl_context, buffer_size) {}

  HttpProxySession::HttpProxySession(net::io_context& ioc, std::shared_ptr<HttpRequestStream> inbound,
                                     std::shared_ptr<HttpResponseStream> downstream, std::size_t buffer_size)
    : inbound_(std::move(inbound)), downstream_(std::move(downstream)),
      upstream_(std::make_shared<HttpClientStream>(ioc)),
      request_buffer_(buffer_size), response_buffer_(buffer_size) {}

  HttpProxySession::HttpProxySession(net::io_context& ioc, std::shared_ptr<HttpRequestStream> inbound,
                                     std::shared_ptr<HttpResponseStream> downstream,
                                     net::ssl::context& ssl_context, std::size_t buffer_size)
    : inbound_(std::move(inbound)), downstream_(std::move(downstream)),
      upstream_(std::make_shared<HttpClientStream>(ioc, ssl_context)),
      request_buffer_(buffer_size), response_buffer_(buffer_size) {}

  void HttpProxySession::start(const std::string& url, HttpClientStream::RequestHead head, CompleteCallback callback)
  {
    complete_ = std::move(callback);
    head.erase(boost::beast::http::field::expect);
    filter_hop_by_hop(head);
    upstream_->async_start(url, std::move(head), [self = shared_from_this()](beast::error_code ec)
    { if (ec) self->finish(ec); else self->pump_request(); });
  }

  void HttpProxySession::pump_request()
  {
    inbound_->async_read_some(net::buffer(request_buffer_), [self = shared_from_this()]
      (beast::error_code ec, std::size_t n, bool done)
    {
      if (ec) return self->finish(ec);
      if (n)
      {
        self->upstream_->async_write_some(net::buffer(self->request_buffer_.data(), n),
          [self, done](beast::error_code write_ec)
          { if (write_ec) self->finish(write_ec); else if (done) self->finish_request(); else self->pump_request(); });
      }
      else if (done) self->finish_request();
      else self->pump_request();
    });
  }

  void HttpProxySession::finish_request()
  {
    upstream_->async_finish_request([self = shared_from_this()](beast::error_code ec)
    { if (ec) self->finish(ec); else self->read_response_head(); });
  }

  void HttpProxySession::read_response_head()
  {
    upstream_->async_read_response_head([self = shared_from_this()]
      (beast::error_code ec, HttpClientStream::ResponseHead head)
    {
      if (ec) return self->finish(ec);
      filter_hop_by_hop(head);
      self->downstream_->async_start(std::move(head), [self](beast::error_code start_ec)
      { if (start_ec) self->finish(start_ec); else self->pump_response(); });
    });
  }

  void HttpProxySession::pump_response()
  {
    upstream_->async_read_some(net::buffer(response_buffer_), [self = shared_from_this()]
      (beast::error_code ec, std::size_t n, bool done)
    {
      if (ec) return self->finish(ec);
      if (n)
      {
        self->downstream_->async_write_some(net::buffer(self->response_buffer_.data(), n),
          [self, done](beast::error_code write_ec)
          {
            if (write_ec) return self->finish(write_ec);
            if (!done) return self->pump_response();
            self->downstream_->async_finish([self](beast::error_code finish_ec) { self->finish(finish_ec); });
          });
      }
      else if (done) self->downstream_->async_finish([self](beast::error_code finish_ec) { self->finish(finish_ec); });
      else self->pump_response();
    });
  }

  void HttpProxySession::cancel()
  {
    if (inbound_) inbound_->cancel();
    if (upstream_) upstream_->cancel();
    if (downstream_) downstream_->cancel();
  }

  void HttpProxySession::finish(beast::error_code ec)
  {
    if (completed_) return;
    completed_ = true;
    if (ec) { spdlog::error("HttpProxySession failed: {}", ec.message()); cancel(); }
    inbound_.reset();
    downstream_.reset();
    if (complete_) complete_(ec);
  }
}
