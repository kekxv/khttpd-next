#include "sse/sse_parser.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>

namespace khttpd::framework::sse
{
  namespace
  {
    std::string single_line(std::string value)
    {
      value.erase(std::remove_if(value.begin(), value.end(), [](const char c) { return c == '\r' || c == '\n'; }), value.end());
      return value;
    }

    void append_data_lines(std::string& output, const std::string& data)
    {
      std::size_t begin = 0;
      while (true)
      {
        const auto end = data.find_first_of("\r\n", begin);
        output += "data: " + data.substr(begin, end == std::string::npos ? end : end - begin) + "\n";
        if (end == std::string::npos) break;
        begin = end + (data[end] == '\r' && end + 1 < data.size() && data[end + 1] == '\n' ? 2 : 1);
      }
    }
  }

  std::string format_sse_event(const SseEvent& event)
  {
    std::string output;
    if (!event.event.empty()) output += "event: " + single_line(event.event) + "\n";
    if (!event.id.empty()) output += "id: " + single_line(event.id) + "\n";
    if (event.retry) output += "retry: " + std::to_string(*event.retry) + "\n";
    append_data_lines(output, event.data);
    output += "\n";
    return output;
  }

  std::string format_sse_comment(const std::string& comment)
  {
    return ": " + single_line(comment) + "\n\n";
  }

  std::vector<SseEvent> SseParser::feed(const std::string_view bytes)
  {
    std::vector<SseEvent> events;
    std::size_t offset = 0;
    const auto configured_chunk_limit = max_event_bytes_ == (std::numeric_limits<std::size_t>::max)()
      ? max_event_bytes_ : max_event_bytes_ + 1;
    const auto chunk_limit = std::max<std::size_t>(configured_chunk_limit, 3);
    while (offset < bytes.size())
    {
      const auto available = chunk_limit - buffer_.size();
      const auto chunk_size = std::min(available, bytes.size() - offset);
      buffer_.append(bytes.data() + offset, chunk_size);
      offset += chunk_size;

      if (skip_leading_lf_ && !buffer_.empty())
      {
        if (buffer_.front() == '\n') buffer_.erase(0, 1);
        skip_leading_lf_ = false;
      }
      if (at_stream_start_)
      {
        constexpr std::string_view utf8_bom{"\xef\xbb\xbf", 3};
        const auto prefix_size = std::min(buffer_.size(), utf8_bom.size());
        if (buffer_.compare(0, prefix_size, utf8_bom.data(), prefix_size) == 0 &&
            buffer_.size() < utf8_bom.size())
          continue;
        if (buffer_.compare(0, utf8_bom.size(), utf8_bom.data(), utf8_bom.size()) == 0)
          buffer_.erase(0, utf8_bom.size());
        at_stream_start_ = false;
      }

      std::size_t consumed = 0;
      while (consumed < buffer_.size())
      {
        const auto newline = buffer_.find_first_of("\r\n", consumed);
        if (newline == std::string::npos) break;
        if (newline - consumed > max_event_bytes_)
          throw std::length_error("SSE line exceeds configured event limit");
        const auto line = buffer_.substr(consumed, newline - consumed);
        process_line(line, events);
        if (buffer_[newline] == '\r' && newline + 1 < buffer_.size() && buffer_[newline + 1] == '\n')
          consumed = newline + 2;
        else
        {
          consumed = newline + 1;
          if (buffer_[newline] == '\r' && consumed == buffer_.size()) skip_leading_lf_ = true;
        }
      }
      if (consumed != 0) buffer_.erase(0, consumed);
      if (!at_stream_start_ && buffer_.size() > max_event_bytes_)
        throw std::length_error("SSE line exceeds configured event limit");
    }
    return events;
  }

  void SseParser::process_line(const std::string& line, std::vector<SseEvent>& events)
  {
    if (line.empty())
    {
      if (!data_.empty())
      {
        data_.pop_back();
        events.push_back({event_.empty() ? "message" : event_, data_, last_event_id_, retry_});
      }
      event_.clear();
      data_.clear();
      retry_.reset();
      return;
    }
    if (line.front() == ':') return;
    const auto colon = line.find(':');
    const auto field = line.substr(0, colon);
    auto value = colon == std::string::npos ? std::string{} : line.substr(colon + 1);
    if (!value.empty() && value.front() == ' ') value.erase(0, 1);
    if (value.size() > max_event_bytes_)
      throw std::length_error("SSE field exceeds configured event limit");
    if (field == "event") event_ = value;
    else if (field == "data")
    {
      if (value.size() >= max_event_bytes_ || data_.size() > max_event_bytes_ - value.size() - 1)
        throw std::length_error("SSE event data exceeds configured event limit");
      data_ += value; data_ += '\n';
    }
    else if (field == "id" && value.find('\0') == std::string::npos) last_event_id_ = value;
    else if (field == "retry" && !value.empty() &&
             std::all_of(value.begin(), value.end(),
                         [](const unsigned char c) { return std::isdigit(c) != 0; }))
    {
      try { retry_ = std::stoull(value); } catch (...) { retry_.reset(); }
    }
  }

  void SseParser::reset()
  {
    buffer_.clear(); event_.clear(); data_.clear(); last_event_id_.clear(); retry_.reset();
    at_stream_start_ = true; skip_leading_lf_ = false;
  }
}
