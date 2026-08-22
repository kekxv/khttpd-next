#ifndef KHTTPD_EXAMPLE_SSE_DEMO_HPP
#define KHTTPD_EXAMPLE_SSE_DEMO_HPP

#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <memory>
#include <optional>

#include <fmt/format.h>

#include "io_context_pool.hpp"
#include "router/http_router.hpp"
#include "sse/sse_session.hpp"

class SseDemo
{
public:
  static void register_routes(khttpd::framework::HttpRouter& router)
  {
    router.sse("/events", [](khttpd::framework::HttpContext&,
                              std::shared_ptr<khttpd::framework::sse::SseSession> session)
    {
      auto state = std::make_shared<State>(khttpd::framework::IoContextPool::instance().get_io_context());
      session->on_close([timer = state->timer](boost::system::error_code)
      {
        timer->cancel();
      });
      session->send({"welcome", R"({"message":"Connected to the khttpd SSE demo"})", "1", 1000});
      publish_tick(state, session);
    });
  }

private:
  struct State
  {
    explicit State(boost::asio::io_context& ioc)
      : timer(std::make_shared<boost::asio::steady_timer>(ioc)) {}

    std::shared_ptr<boost::asio::steady_timer> timer;
    unsigned int sequence = 1;
  };

  static void publish_tick(const std::shared_ptr<State>& state,
                           const std::shared_ptr<khttpd::framework::sse::SseSession>& session)
  {
    if (!session->is_open()) return;
    if (!session->send({"tick", fmt::format(R"({{"sequence":{}}})", state->sequence++), "", std::nullopt}))
    {
      session->close();
      return;
    }
    state->timer->expires_after(std::chrono::seconds(1));
    state->timer->async_wait([state, session](boost::system::error_code ec)
    {
      if (!ec) publish_tick(state, session);
    });
  }
};

#endif
