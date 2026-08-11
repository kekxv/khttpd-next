#ifndef KHTTPD_FRAMEWORK_CONTEXT_HTTP_RESPONSE_STREAM_HPP
#define KHTTPD_FRAMEWORK_CONTEXT_HTTP_RESPONSE_STREAM_HPP

#include <boost/asio/buffer.hpp>
#include <boost/beast/http.hpp>
#include <functional>

namespace khttpd::framework
{
  class HttpResponseStream
  {
  public:
    using ResponseHead = boost::beast::http::response<boost::beast::http::empty_body>;
    using Callback = std::function<void(boost::system::error_code)>;
    virtual ~HttpResponseStream() = default;
    virtual void async_start(ResponseHead head, Callback callback) = 0;
    virtual void async_write_some(boost::asio::const_buffer buffer, Callback callback) = 0;
    virtual void async_finish(Callback callback) = 0;
    // Stops an inbound request-body read while preserving this response stream.
    virtual void cancel_request_body() { cancel(); }
    virtual void cancel() = 0;
  };
}

#endif
