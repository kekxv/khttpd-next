#ifndef KHTTPD_FRAMEWORK_CLIENT_HTTP_CLIENT_STREAM_HPP
#define KHTTPD_FRAMEWORK_CLIENT_HTTP_CLIENT_STREAM_HPP

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <functional>
#include <memory>
#include <string>

namespace khttpd::framework::client
{
  class HttpClientStream : public std::enable_shared_from_this<HttpClientStream>
  {
  public:
    using RequestHead = boost::beast::http::request<boost::beast::http::empty_body>;
    using ResponseHead = boost::beast::http::response<boost::beast::http::empty_body>;
    using Callback = std::function<void(boost::system::error_code)>;
    using ResponseHeadCallback = std::function<void(boost::system::error_code, ResponseHead)>;
    using ReadCallback = std::function<void(boost::system::error_code, std::size_t, bool)>;

    HttpClientStream();
    explicit HttpClientStream(boost::asio::ssl::context& ssl_context);
    explicit HttpClientStream(boost::asio::io_context& ioc);
    HttpClientStream(boost::asio::io_context& ioc, boost::asio::ssl::context& ssl_context);
    ~HttpClientStream();
    void async_start(const std::string& url, RequestHead head, Callback callback);
    void async_write_some(boost::asio::const_buffer buffer, Callback callback);
    void async_finish_request(Callback callback);
    void async_read_response_head(ResponseHeadCallback callback);
    void async_read_some(boost::asio::mutable_buffer buffer, ReadCallback callback);
    void cancel();

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
  };
}

#endif
