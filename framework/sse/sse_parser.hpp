#ifndef KHTTPD_FRAMEWORK_SSE_SSE_PARSER_HPP
#define KHTTPD_FRAMEWORK_SSE_SSE_PARSER_HPP

#include "sse/sse_event.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace khttpd::framework::sse
{
  class SseParser
  {
  public:
    std::vector<SseEvent> feed(std::string_view bytes);
    void reset();

  private:
    void process_line(const std::string& line, std::vector<SseEvent>& events);
    std::string buffer_;
    std::string event_;
    std::string data_;
    std::string last_event_id_;
    std::optional<std::uint64_t> retry_;
    bool at_stream_start_ = true;
    bool skip_leading_lf_ = false;
  };
}

#endif
