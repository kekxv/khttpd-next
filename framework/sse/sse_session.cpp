#include "sse/sse_session.hpp"

#include <boost/asio/buffer.hpp>
#include <deque>
#include <exception>
#include <mutex>
#include <utility>

#include <spdlog/spdlog.h>

namespace khttpd::framework::sse
{
  struct SseSession::Impl : std::enable_shared_from_this<Impl>
  {
    std::shared_ptr<HttpResponseStream> response;
    int version;
    bool keep_alive;
    mutable std::mutex mutex;
    std::deque<std::shared_ptr<std::string>> queue;
    std::size_t pending_bytes = 0;
    std::size_t max_pending_bytes;
    bool start_requested = false;
    bool started = false;
    bool writing = false;
    bool closing = false;
    bool closed = false;
    boost::system::error_code close_error;
    CloseHandler close_handler;

    Impl(std::shared_ptr<HttpResponseStream> value, const int v, const bool keep,
         const std::size_t maximum_pending_bytes)
      : response(std::move(value)), version(v), keep_alive(keep),
        max_pending_bytes(maximum_pending_bytes) {}

    void start()
    {
      {
        std::lock_guard lock(mutex);
        if (start_requested || closed) return;
        start_requested = true;
      }
      HttpResponseStream::ResponseHead head{boost::beast::http::status::ok, version};
      head.keep_alive(keep_alive);
      head.set(boost::beast::http::field::content_type, "text/event-stream");
      head.set(boost::beast::http::field::cache_control, "no-cache");
      head.set("X-Accel-Buffering", "no");
      response->async_start(std::move(head), [self = shared_from_this()](boost::system::error_code ec)
      {
        if (ec) return self->finish(ec);
        { std::lock_guard lock(self->mutex); self->started = true; }
        self->write_next();
      });
    }

    bool enqueue(std::string value)
    {
      {
        std::lock_guard lock(mutex);
        if (closing || closed) return false;
        if (value.size() > max_pending_bytes || pending_bytes > max_pending_bytes - value.size()) return false;
        pending_bytes += value.size();
        queue.push_back(std::make_shared<std::string>(std::move(value)));
      }
      write_next();
      return true;
    }

    void write_next()
    {
      std::shared_ptr<std::string> value;
      bool should_finish = false;
      {
        std::lock_guard lock(mutex);
        if (!started || writing || closed) return;
        if (queue.empty())
        {
          if (!closing) return;
          writing = true;
          should_finish = true;
        }
        else
        {
          writing = true;
          value = queue.front();
        }
      }
      if (should_finish)
      {
        response->async_finish([self = shared_from_this()](boost::system::error_code ec) { self->finish(ec); });
        return;
      }
      response->async_write_some(boost::asio::buffer(*value),
        [self = shared_from_this(), value](boost::system::error_code ec)
        {
          if (ec) return self->finish(ec);
          {
            std::lock_guard lock(self->mutex);
            if (!self->queue.empty())
            {
              self->pending_bytes -= self->queue.front()->size();
              self->queue.pop_front();
            }
            self->writing = false;
          }
          self->write_next();
        });
    }

    void finish(boost::system::error_code ec)
    {
      CloseHandler handler;
      {
        std::lock_guard lock(mutex);
        if (closed) return;
        closed = true;
        close_error = ec;
        writing = false;
        pending_bytes = 0;
        queue.clear();
        handler = std::move(close_handler);
      }
      invoke_close_handler(handler, ec);
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
        spdlog::error("SSE session close callback failed: {}", error.what());
      }
      catch (...)
      {
        spdlog::error("SSE session close callback failed with a non-standard exception");
      }
    }
  };

  SseSession::SseSession(std::shared_ptr<HttpResponseStream> response, const int version, const bool keep_alive,
                         const std::size_t max_pending_bytes)
    : impl_(std::make_shared<Impl>(std::move(response), version, keep_alive, max_pending_bytes)) {}
  void SseSession::start() { impl_->start(); }
  bool SseSession::send(SseEvent event) { return impl_->enqueue(format_sse_event(event)); }
  bool SseSession::send_comment(std::string comment) { return impl_->enqueue(format_sse_comment(comment)); }
  void SseSession::close() { { std::lock_guard lock(impl_->mutex); impl_->closing = true; } impl_->write_next(); }
  void SseSession::cancel() { impl_->response->cancel(); impl_->finish(boost::asio::error::operation_aborted); }
  void SseSession::on_close(CloseHandler handler)
  {
    boost::system::error_code error;
    bool already_closed = false;
    {
      std::lock_guard lock(impl_->mutex);
      already_closed = impl_->closed;
      error = impl_->close_error;
      if (!already_closed) impl_->close_handler = std::move(handler);
    }
    if (already_closed) Impl::invoke_close_handler(handler, error);
  }
  bool SseSession::is_open() const { std::lock_guard lock(impl_->mutex); return !impl_->closing && !impl_->closed; }
}
