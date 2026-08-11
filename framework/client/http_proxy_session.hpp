#ifndef KHTTPD_FRAMEWORK_CLIENT_HTTP_PROXY_SESSION_HPP
#define KHTTPD_FRAMEWORK_CLIENT_HTTP_PROXY_SESSION_HPP

#include "http_client_stream.hpp"
#include "context/http_request_stream.hpp"
#include "context/http_response_stream.hpp"
#include <vector>

namespace khttpd::framework::client
{
  class HttpProxySession : public std::enable_shared_from_this<HttpProxySession>
  {
  public:
    using CompleteCallback = std::function<void(boost::system::error_code)>;
    HttpProxySession(std::shared_ptr<HttpRequestStream> inbound,
                     std::shared_ptr<HttpResponseStream> downstream,
                     std::size_t buffer_size = 64 * 1024);
    HttpProxySession(std::shared_ptr<HttpRequestStream> inbound,
                     std::shared_ptr<HttpResponseStream> downstream,
                     boost::asio::ssl::context& ssl_context,
                     std::size_t buffer_size = 64 * 1024);
    HttpProxySession(boost::asio::io_context& ioc,
                     std::shared_ptr<HttpRequestStream> inbound,
                     std::shared_ptr<HttpResponseStream> downstream,
                     std::size_t buffer_size = 64 * 1024);
    HttpProxySession(boost::asio::io_context& ioc,
                     std::shared_ptr<HttpRequestStream> inbound,
                     std::shared_ptr<HttpResponseStream> downstream,
                     boost::asio::ssl::context& ssl_context,
                     std::size_t buffer_size = 64 * 1024);
    void start(const std::string& upstream_url, HttpClientStream::RequestHead head,
               CompleteCallback callback = {});
    void cancel();

  private:
    std::shared_ptr<HttpRequestStream> inbound_;
    std::shared_ptr<HttpResponseStream> downstream_;
    std::shared_ptr<HttpClientStream> upstream_;
    std::vector<char> request_buffer_;
    std::vector<char> response_buffer_;
    CompleteCallback complete_;
    bool completed_ = false;
    void pump_request();
    void finish_request();
    void read_response_head();
    void pump_response();
    void finish(boost::system::error_code ec);
  };
}

#endif
