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
    router.get("/events-demo", [](khttpd::framework::HttpContext& ctx)
    {
      ctx.set_status(boost::beast::http::status::ok);
      ctx.set_content_type("text/html; charset=utf-8");
      ctx.set_body(demo_page());
    }, {"SSE browser demo", "Displays connection state and events received from the example SSE stream."});

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
  static const char* demo_page()
  {
    return R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>khttpd SSE demo</title>
  <style>
    :root { color-scheme: light; font-family: Inter, ui-sans-serif, system-ui, sans-serif; }
    * { box-sizing: border-box; }
    body { margin: 0; background: #f5f7f8; color: #182026; }
    main { width: min(760px, calc(100% - 32px)); margin: 48px auto; }
    header { display: flex; align-items: flex-start; justify-content: space-between; gap: 24px; margin-bottom: 24px; }
    h1 { margin: 0 0 8px; font-size: 28px; letter-spacing: 0; }
    p { margin: 0; color: #5b6670; line-height: 1.6; }
    a { color: #1769aa; text-decoration: none; }
    a:hover { text-decoration: underline; }
    .status { display: inline-flex; align-items: center; gap: 8px; min-width: 132px; padding: 8px 12px;
      border: 1px solid #d8dee3; border-radius: 6px; background: #fff; font-size: 14px; }
    .status-dot { width: 9px; height: 9px; border-radius: 50%; background: #9aa4ad; }
    .status.connected .status-dot { background: #16834b; }
    .status.disconnected .status-dot { background: #c43d3d; }
    .toolbar { display: flex; align-items: center; justify-content: space-between; gap: 16px; padding: 12px 0;
      border-top: 1px solid #d8dee3; border-bottom: 1px solid #d8dee3; }
    .actions { display: flex; gap: 8px; }
    button { min-height: 36px; padding: 0 14px; border: 1px solid #bdc6cd; border-radius: 6px;
      background: #fff; color: #182026; font: inherit; cursor: pointer; }
    button:hover { background: #eef2f4; }
    code { color: #34404a; }
    #event-list { min-height: 260px; margin: 0; padding: 0; list-style: none; background: #fff; }
    .event { display: grid; grid-template-columns: 90px 1fr auto; gap: 16px; align-items: start;
      padding: 14px 16px; border-bottom: 1px solid #e5e9ec; }
    .event-type { color: #1769aa; font-weight: 650; }
    .event-data { margin: 0; overflow-wrap: anywhere; white-space: pre-wrap; font: 13px/1.5 ui-monospace, monospace; }
    .event-time { color: #77828c; font-size: 12px; }
    .empty { padding: 72px 16px; color: #77828c; text-align: center; }
    @media (max-width: 560px) {
      main { margin: 24px auto; }
      header { flex-direction: column; }
      .event { grid-template-columns: 72px 1fr; }
      .event-time { grid-column: 2; }
      .toolbar { align-items: flex-start; flex-direction: column; }
    }
  </style>
</head>
<body>
  <main>
    <header>
      <div>
        <h1>Server-Sent Events</h1>
        <p>Live events from <code>/events</code>. <a href="/">Back to examples</a></p>
      </div>
      <div id="connection-status" class="status" role="status" aria-live="polite">
        <span class="status-dot" aria-hidden="true"></span><span id="status-label">Connecting</span>
      </div>
    </header>
    <div class="toolbar">
      <span id="event-count">0 events</span>
      <div class="actions">
        <button id="reconnect" type="button">Reconnect</button>
        <button id="clear" type="button">Clear</button>
      </div>
    </div>
    <ul id="event-list" aria-live="polite"><li class="empty">Waiting for events...</li></ul>
  </main>
  <script>
    const statusNode = document.getElementById('connection-status');
    const statusLabel = document.getElementById('status-label');
    const eventList = document.getElementById('event-list');
    const eventCount = document.getElementById('event-count');
    let source;
    let count = 0;

    function setStatus(label, state) {
      statusLabel.textContent = label;
      statusNode.className = 'status ' + state;
    }

    function appendEvent(event) {
      const empty = eventList.querySelector('.empty');
      if (empty) empty.remove();
      const row = document.createElement('li');
      row.className = 'event';
      const type = document.createElement('span');
      type.className = 'event-type';
      type.textContent = event.type;
      const data = document.createElement('pre');
      data.className = 'event-data';
      data.textContent = event.data;
      const time = document.createElement('time');
      time.className = 'event-time';
      time.textContent = new Date().toLocaleTimeString();
      row.append(type, data, time);
      eventList.prepend(row);
      while (eventList.children.length > 100) eventList.lastElementChild.remove();
      count += 1;
      eventCount.textContent = count + (count === 1 ? ' event' : ' events');
    }

    function connect() {
      if (source) source.close();
      setStatus('Connecting', '');
      source = new EventSource('/events');
      source.onopen = () => setStatus('Connected', 'connected');
      source.onerror = () => setStatus('Reconnecting', 'disconnected');
      source.addEventListener('welcome', appendEvent);
      source.addEventListener('tick', appendEvent);
      source.onmessage = appendEvent;
    }

    document.getElementById('reconnect').addEventListener('click', connect);
    document.getElementById('clear').addEventListener('click', () => {
      eventList.replaceChildren();
      count = 0;
      eventCount.textContent = '0 events';
      const empty = document.createElement('li');
      empty.className = 'empty';
      empty.textContent = 'Waiting for events...';
      eventList.append(empty);
    });
    window.addEventListener('beforeunload', () => source && source.close());
    connect();
  </script>
</body>
</html>)HTML";
  }

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
