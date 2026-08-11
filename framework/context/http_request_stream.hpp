#ifndef KHTTPD_FRAMEWORK_CONTEXT_HTTP_REQUEST_STREAM_HPP
#define KHTTPD_FRAMEWORK_CONTEXT_HTTP_REQUEST_STREAM_HPP

#include <boost/asio/buffer.hpp>
#include <boost/system/error_code.hpp>
#include <functional>

namespace khttpd::framework
{
  // A request body is consumed serially: callers must wait for a read callback
  // before issuing the next read. This is the backpressure boundary used by
  // streaming proxy handlers.
  class HttpRequestStream
  {
  public:
    using ReadCallback = std::function<void(boost::system::error_code, std::size_t, bool)>;
    virtual ~HttpRequestStream() = default;

    virtual void async_read_some(boost::asio::mutable_buffer buffer, ReadCallback callback) = 0;
    // Cancels only request-body consumption. The response side remains usable.
    virtual void cancel_read() { cancel(); }
    virtual void cancel() = 0;
  };
}

#endif
