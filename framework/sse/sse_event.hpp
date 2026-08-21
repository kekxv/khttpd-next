#ifndef KHTTPD_FRAMEWORK_SSE_SSE_EVENT_HPP
#define KHTTPD_FRAMEWORK_SSE_SSE_EVENT_HPP

#include <cstdint>
#include <optional>
#include <string>

namespace khttpd::framework::sse
{
  struct SseEvent
  {
    std::string event;
    std::string data;
    std::string id;
    std::optional<std::uint64_t> retry;
  };

  std::string format_sse_event(const SseEvent& event);
  std::string format_sse_comment(const std::string& comment);
}

#endif
