#ifndef KHTTPD_FRAMEWORK_SSE_SSE_SESSION_HPP
#define KHTTPD_FRAMEWORK_SSE_SSE_SESSION_HPP

#include "context/http_response_stream.hpp"
#include "sse/sse_event.hpp"

#include <functional>
#include <memory>
#include <string>

namespace khttpd::framework::sse
{
  class SseSession : public std::enable_shared_from_this<SseSession>
  {
  public:
    using CloseHandler = std::function<void(boost::system::error_code)>;

    SseSession(std::shared_ptr<HttpResponseStream> response, int http_version, bool keep_alive);
    void start();
    bool send(SseEvent event);
    bool send_comment(std::string comment);
    void close();
    void cancel();
    void on_close(CloseHandler handler);
    bool is_open() const;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
  };
}

#endif
