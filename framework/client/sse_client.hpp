#ifndef KHTTPD_FRAMEWORK_CLIENT_SSE_CLIENT_HPP
#define KHTTPD_FRAMEWORK_CLIENT_SSE_CLIENT_HPP

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/system/error_code.hpp>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "sse/sse_event.hpp"

namespace khttpd::framework::client
{
  class SseClient
  {
  public:
    using EventHandler = std::function<void(const sse::SseEvent&)>;
    using CloseHandler = std::function<void(boost::system::error_code)>;

    explicit SseClient(boost::asio::io_context& ioc,
                       std::size_t max_event_bytes = 1024 * 1024);
    SseClient(boost::asio::io_context& ioc, boost::asio::ssl::context& ssl_context,
              std::size_t max_event_bytes = 1024 * 1024);
    ~SseClient();

    SseClient(const SseClient&) = delete;
    SseClient& operator=(const SseClient&) = delete;

    void connect(const std::string& url,
                 const std::map<std::string, std::string>& headers,
                 EventHandler on_event,
                 CloseHandler on_close);
    void cancel();

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
  };
}

#endif
