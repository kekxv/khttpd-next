#include "client/sse_client.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/beast/http.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <utility>

#include <spdlog/spdlog.h>

#include "client/http_client_stream.hpp"
#include "sse/sse_parser.hpp"

namespace khttpd::framework::client
{
  namespace http = boost::beast::http;
  namespace net = boost::asio;

  namespace
  {
    bool is_event_stream_content_type(const boost::beast::string_view value)
    {
      std::string media_type(value.data(), value.size());
      const auto parameters = media_type.find(';');
      if (parameters != std::string::npos) media_type.erase(parameters);
      while (!media_type.empty() && std::isspace(static_cast<unsigned char>(media_type.back())))
        media_type.pop_back();
      auto first = media_type.begin();
      while (first != media_type.end() && std::isspace(static_cast<unsigned char>(*first))) ++first;
      media_type.erase(media_type.begin(), first);
      std::transform(media_type.begin(), media_type.end(), media_type.begin(),
                     [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return media_type == "text/event-stream";
    }
  }

  struct SseClient::Impl : std::enable_shared_from_this<Impl>
  {
    std::shared_ptr<HttpClientStream> stream;
    sse::SseParser parser;
    std::array<char, 8192> read_buffer{};
    EventHandler event_handler;
    CloseHandler close_handler;
    std::mutex mutex;
    bool connect_started = false;
    bool terminal = false;

    Impl(net::io_context& ioc, const std::size_t max_event_bytes)
      : stream(std::make_shared<HttpClientStream>(ioc)), parser(max_event_bytes) {}
    Impl(net::io_context& ioc, net::ssl::context& context, const std::size_t max_event_bytes)
      : stream(std::make_shared<HttpClientStream>(ioc, context)), parser(max_event_bytes) {}

    void connect(const std::string& url,
                 const std::map<std::string, std::string>& headers,
                 EventHandler on_event,
                 CloseHandler on_close)
    {
      bool duplicate = false;
      {
        std::lock_guard lock(mutex);
        if (connect_started)
          duplicate = true;
        else
        {
          connect_started = true;
          event_handler = std::move(on_event);
          close_handler = std::move(on_close);
        }
      }
      if (duplicate)
      {
        invoke_close_handler(on_close, make_error_code(boost::system::errc::operation_in_progress));
        return;
      }

      HttpClientStream::RequestHead request{http::verb::get, "/", 11};
      request.keep_alive(true);
      for (const auto& [name, value] : headers) request.set(name, value);
      request.set(http::field::accept, "text/event-stream");
      request.set(http::field::cache_control, "no-cache");
      stream->async_start(url, std::move(request),
        [self = shared_from_this()](boost::system::error_code ec)
        {
          if (ec) return self->finish(ec);
          self->stream->async_finish_request([self](boost::system::error_code finish_ec)
          {
            if (finish_ec) return self->finish(finish_ec);
            self->read_head();
          });
        });
    }

    void read_head()
    {
      stream->async_read_response_head(
        [self = shared_from_this()](boost::system::error_code ec, HttpClientStream::ResponseHead head)
        {
          if (ec) return self->finish(ec);
          if (head.result() != http::status::ok ||
              !is_event_stream_content_type(head[http::field::content_type]))
            return self->finish(make_error_code(boost::system::errc::protocol_error));
          self->read_next();
        });
    }

    void read_next()
    {
      stream->async_read_some(net::buffer(read_buffer),
        [self = shared_from_this()](boost::system::error_code ec, const std::size_t size, const bool done)
        {
          if (size != 0)
          {
            try
            {
              const auto events = self->parser.feed(std::string_view(self->read_buffer.data(), size));
              for (const auto& event : events) self->emit(event);
            }
            catch (const std::length_error&)
            {
              return self->finish(net::error::message_size);
            }
          }
          if (self->is_terminal()) return;
          if (done) return self->finish({});
          if (ec == net::error::eof || ec == http::error::end_of_stream) return self->finish({});
          if (ec) return self->finish(ec);
          self->read_next();
        });
    }

    void emit(const sse::SseEvent& event)
    {
      EventHandler handler;
      {
        std::lock_guard lock(mutex);
        if (terminal) return;
        handler = event_handler;
      }
      if (!handler) return;
      try
      {
        handler(event);
      }
      catch (const std::exception& error)
      {
        spdlog::error("SSE event callback failed: {}", error.what());
        finish(net::error::operation_aborted);
      }
      catch (...)
      {
        spdlog::error("SSE event callback failed with a non-standard exception");
        finish(net::error::operation_aborted);
      }
    }

    bool is_terminal()
    {
      std::lock_guard lock(mutex);
      return terminal;
    }

    void finish(const boost::system::error_code ec)
    {
      CloseHandler handler;
      {
        std::lock_guard lock(mutex);
        if (terminal) return;
        terminal = true;
        handler = std::move(close_handler);
        event_handler = {};
      }
      if (ec) stream->cancel();
      invoke_close_handler(handler, ec);
    }

    void cancel()
    {
      stream->cancel();
      finish(net::error::operation_aborted);
    }

    void abandon()
    {
      {
        std::lock_guard lock(mutex);
        terminal = true;
        event_handler = {};
        close_handler = {};
      }
      stream->cancel();
    }

    static void invoke_close_handler(const CloseHandler& handler, const boost::system::error_code ec)
    {
      if (!handler) return;
      try
      {
        handler(ec);
      }
      catch (const std::exception& error)
      {
        spdlog::error("SSE close callback failed: {}", error.what());
      }
      catch (...)
      {
        spdlog::error("SSE close callback failed with a non-standard exception");
      }
    }
  };

  SseClient::SseClient(net::io_context& ioc, const std::size_t max_event_bytes)
    : impl_(std::make_shared<Impl>(ioc, max_event_bytes)) {}
  SseClient::SseClient(net::io_context& ioc, net::ssl::context& context,
                       const std::size_t max_event_bytes)
    : impl_(std::make_shared<Impl>(ioc, context, max_event_bytes)) {}
  SseClient::~SseClient() { if (impl_) impl_->abandon(); }
  void SseClient::connect(const std::string& url,
                          const std::map<std::string, std::string>& headers,
                          EventHandler on_event,
                          CloseHandler on_close)
  {
    impl_->connect(url, headers, std::move(on_event), std::move(on_close));
  }
  void SseClient::cancel() { impl_->cancel(); }
}
