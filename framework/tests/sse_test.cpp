#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "client/sse_client.hpp"
#include "sse/sse_event.hpp"
#include "sse/sse_parser.hpp"
#include "sse/sse_session.hpp"
#include "http_session_test_harness.hpp"

namespace sse = khttpd::framework::sse;
namespace fw = khttpd::framework;
namespace test = khttpd::framework::tests;
namespace http = boost::beast::http;
namespace net = boost::asio;
namespace client = khttpd::framework::client;
using tcp = net::ip::tcp;

namespace
{
  class ImmediateResponseStream final : public fw::HttpResponseStream
  {
  public:
    bool finished = false;
    bool disconnect_wait_started = false;

    void async_start(ResponseHead, Callback callback) override { callback({}); }
    void async_write_some(net::const_buffer, Callback callback) override { callback({}); }
    void async_finish(Callback callback) override
    {
      finished = true;
      callback({});
    }
    void async_wait_disconnect(Callback) override { disconnect_wait_started = true; }
    void cancel() override {}
  };
}

TEST(SseParserTest, PreservesAFragmentedMultilineEvent)
{
  sse::SseParser parser;
  EXPECT_TRUE(parser.feed("event: con").empty());
  EXPECT_TRUE(parser.feed("fig\r\nid: 42\r\ndata: first\r\n").empty());
  const auto events = parser.feed("data: second\r\nretry: 1500\r\n\r\n");

  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].event, "config");
  EXPECT_EQ(events[0].id, "42");
  EXPECT_EQ(events[0].data, "first\nsecond");
  ASSERT_TRUE(events[0].retry.has_value());
  EXPECT_EQ(*events[0].retry, 1500U);
}

TEST(SseParserTest, IgnoresCommentsAndInvalidRetryWithoutLosingData)
{
  sse::SseParser parser;
  const auto events = parser.feed(": heartbeat\nretry: soon\ndata: ready\n\n");
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].data, "ready");
  EXPECT_FALSE(events[0].retry.has_value());
}

TEST(SseParserTest, AcceptsFragmentedUtf8BomAndStandaloneCarriageReturns)
{
  sse::SseParser parser;
  EXPECT_TRUE(parser.feed("\xef").empty());
  EXPECT_TRUE(parser.feed("\xbb\xbf" "data: one\rdata: two\r").empty());
  const auto events = parser.feed("\r");

  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].event, "message");
  EXPECT_EQ(events[0].data, "one\ntwo");
}

TEST(SseEventTest, FormatsEveryDataLineAndOptionalFields)
{
  sse::SseEvent event{"instances", "one\ntwo", "9", 2500};
  EXPECT_EQ(sse::format_sse_event(event),
            "event: instances\nid: 9\nretry: 2500\ndata: one\ndata: two\n\n");
}

TEST(SseEventTest, BareCarriageReturnsCannotInjectEventFields)
{
  sse::SseEvent event{"config", "safe\rid: injected\r\nretry: 1", "trusted", std::nullopt};
  EXPECT_EQ(sse::format_sse_event(event),
            "event: config\nid: trusted\ndata: safe\ndata: id: injected\ndata: retry: 1\n\n");
}

TEST(SseParserTest, RejectsAnEventThatExceedsItsConfiguredMemoryLimit)
{
  sse::SseParser parser(32);
  EXPECT_THROW(parser.feed("data: " + std::string(33, 'x') + "\n"), std::length_error);
}

TEST(SseParserTest, LimitAppliesPerEventRatherThanPerNetworkRead)
{
  sse::SseParser parser(32);
  const auto events = parser.feed("data: one\n\ndata: two\n\ndata: three\n\n");

  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(events[0].data, "one");
  EXPECT_EQ(events[1].data, "two");
  EXPECT_EQ(events[2].data, "three");
}

TEST(SseParserTest, MinimalLimitStillConsumesAFragmentedUtf8Bom)
{
  sse::SseParser parser(1);
  EXPECT_TRUE(parser.feed("\xef").empty());
  EXPECT_TRUE(parser.feed("\xbb").empty());
  EXPECT_TRUE(parser.feed("\xbf").empty());
  EXPECT_THROW(parser.feed("ab"), std::length_error);
}

TEST(SseSessionTest, StreamsQueuedEventsInOrderWithSseHeaders)
{
  test::TempWebRoot web_root;
  fw::HttpRouter router;
  fw::WebsocketRouter websocket_router;
  router.sse("/events", [](fw::HttpContext&, std::shared_ptr<sse::SseSession> session)
  {
    session->send({"config", "first", "1", std::nullopt});
    session->send({"instances", "second", "2", std::nullopt});
    session->close();
  });
  http::request<http::string_body> request{http::verb::get, "/events", 11};
  request.keep_alive(false);
  const auto response = test::round_trip(router, websocket_router, web_root.path, std::move(request));

  EXPECT_EQ(response.result(), http::status::ok);
  EXPECT_EQ(response[http::field::content_type], "text/event-stream");
  EXPECT_EQ(response[http::field::cache_control], "no-cache");
  EXPECT_EQ(response["X-Accel-Buffering"], "no");
  EXPECT_EQ(response.body(), "event: config\nid: 1\ndata: first\n\nevent: instances\nid: 2\ndata: second\n\n");
}

TEST(SseSessionTest, RejectsWritesBeyondTheConfiguredQueueLimit)
{
  test::TempWebRoot web_root;
  fw::HttpRouter router;
  fw::WebsocketRouter websocket_router;
  bool accepted = true;
  router.sse("/events", [&](fw::HttpContext&, std::shared_ptr<sse::SseSession> session)
  {
    accepted = session->send({"message", std::string(64, 'x'), "", std::nullopt});
    session->close();
  }, 32);
  http::request<http::string_body> request{http::verb::get, "/events", 11};
  request.keep_alive(false);
  const auto response = test::round_trip(router, websocket_router, web_root.path, std::move(request));

  EXPECT_FALSE(accepted);
  EXPECT_TRUE(response.body().empty());
}

TEST(SseSessionTest, RequestBodiesForceConnectionCloseToPreventDesynchronization)
{
  test::TempWebRoot web_root;
  fw::HttpRouter router;
  fw::WebsocketRouter websocket_router;
  router.sse("/events", [](fw::HttpContext&, std::shared_ptr<sse::SseSession> session)
  {
    session->send({"message", "ready", "", std::nullopt});
    session->close();
  });
  http::request<http::string_body> request{http::verb::get, "/events", 11};
  request.body() = "unconsumed-body";
  request.prepare_payload();
  request.keep_alive(true);
  const auto response = test::round_trip(router, websocket_router, web_root.path, std::move(request));

  EXPECT_FALSE(response.keep_alive());
  EXPECT_EQ(response.body(), "event: message\ndata: ready\n\n");
}

TEST(SseSessionTest, PreRequestInterceptorCanDenyTheStreamBeforeItsHandlerRuns)
{
  class DenyInterceptor final : public fw::Interceptor
  {
  public:
    fw::InterceptorResult handle_request(fw::HttpContext& ctx) override
    {
      ctx.set_status(http::status::forbidden);
      ctx.set_body("denied");
      return fw::InterceptorResult::Stop;
    }
  };

  test::TempWebRoot web_root;
  fw::HttpRouter router;
  fw::WebsocketRouter websocket_router;
  bool handler_called = false;
  router.add_interceptor(std::make_shared<DenyInterceptor>());
  router.sse("/events", [&](fw::HttpContext&, std::shared_ptr<sse::SseSession>)
  {
    handler_called = true;
  });
  http::request<http::string_body> request{http::verb::get, "/events", 11};
  request.keep_alive(false);
  const auto response = test::round_trip(router, websocket_router, web_root.path, std::move(request));

  EXPECT_EQ(response.result(), http::status::forbidden);
  EXPECT_EQ(response.body(), "denied");
  EXPECT_FALSE(handler_called);
}

TEST(SseSessionTest, ThrowingHandlerReturnsOneSafeErrorResponse)
{
  test::TempWebRoot web_root;
  fw::HttpRouter router;
  fw::WebsocketRouter websocket_router;
  router.sse("/events", [](fw::HttpContext&, std::shared_ptr<sse::SseSession>)
  {
    throw std::runtime_error("sensitive SSE failure");
  });
  http::request<http::string_body> request{http::verb::get, "/events", 11};
  request.keep_alive(false);
  const auto response = test::round_trip(router, websocket_router, web_root.path, std::move(request));

  EXPECT_EQ(response.result(), http::status::internal_server_error);
  EXPECT_EQ(response[http::field::content_type], "application/json");
  EXPECT_EQ(response.body(), R"({"code":"INTERNAL_SERVER_ERROR","message":"Internal server error"})");
  EXPECT_EQ(response.body().find("sensitive SSE failure"), std::string::npos);
}

TEST(SseSessionTest, ExplicitStartInsideRouteHandlerIsIdempotent)
{
  test::TempWebRoot web_root;
  fw::HttpRouter router;
  fw::WebsocketRouter websocket_router;
  router.sse("/events", [](fw::HttpContext&, std::shared_ptr<sse::SseSession> session)
  {
    session->start();
    session->send({"message", "ready", "", std::nullopt});
    session->close();
  });
  http::request<http::string_body> request{http::verb::get, "/events", 11};
  request.keep_alive(false);
  const auto response = test::round_trip(router, websocket_router, web_root.path, std::move(request));

  EXPECT_EQ(response.result(), http::status::ok);
  EXPECT_EQ(response.body(), "event: message\ndata: ready\n\n");
}

TEST(SseSessionTest, ContainsExceptionsThrownByCloseCallback)
{
  auto response = std::make_shared<ImmediateResponseStream>();
  sse::SseSession session(response, 11, false);
  session.on_close([](boost::system::error_code) { throw std::runtime_error("close callback failure"); });
  session.close();

  EXPECT_NO_THROW(session.start());
  EXPECT_TRUE(response->finished);
}

TEST(SseSessionTest, GracefulCloseBeforeStartDoesNotArmADisconnectRead)
{
  auto response = std::make_shared<ImmediateResponseStream>();
  sse::SseSession session(response, 11, true);
  session.close();

  session.start();

  EXPECT_TRUE(response->finished);
  EXPECT_FALSE(response->disconnect_wait_started);
}

TEST(SseSessionTest, DetectsAPassiveClientDisconnectWithoutAnotherWrite)
{
  test::TempWebRoot web_root;
  fw::HttpRouter router;
  fw::WebsocketRouter websocket_router;
  std::mutex mutex;
  std::condition_variable closed_condition;
  std::shared_ptr<sse::SseSession> server_session;
  bool closed = false;
  router.sse("/events", [&](fw::HttpContext&, std::shared_ptr<sse::SseSession> session)
  {
    {
      std::lock_guard lock(mutex);
      server_session = session;
    }
    session->on_close([&](boost::system::error_code)
    {
      {
        std::lock_guard lock(mutex);
        closed = true;
      }
      closed_condition.notify_one();
    });
  });

  net::io_context server_ioc;
  auto guard = net::make_work_guard(server_ioc);
  tcp::acceptor acceptor(server_ioc, {net::ip::address_v4::loopback(), 0});
  const auto endpoint = acceptor.local_endpoint();
  acceptor.async_accept([&](boost::system::error_code ec, tcp::socket socket)
  {
    if (ec) return;
    std::make_shared<fw::HttpSession>(std::move(socket), router, websocket_router,
                                      web_root.path.string(), boost::filesystem::canonical(web_root.path))->run();
  });
  std::thread server_thread([&] { server_ioc.run(); });

  net::io_context client_ioc;
  tcp::socket client(client_ioc);
  client.connect(endpoint);
  http::request<http::empty_body> request{http::verb::get, "/events", 11};
  request.set(http::field::host, "localhost");
  request.keep_alive(true);
  http::write(client, request);
  boost::beast::flat_buffer response_buffer;
  http::response_parser<http::empty_body> response_parser;
  http::read_header(client, response_buffer, response_parser);
  boost::system::error_code ignored;
  client.shutdown(tcp::socket::shutdown_both, ignored);
  client.close(ignored);

  std::unique_lock lock(mutex);
  const bool detected = closed_condition.wait_for(lock, std::chrono::milliseconds(500), [&] { return closed; });
  auto session = server_session;
  server_session.reset();
  lock.unlock();
  if (session && session->is_open()) session->cancel();
  session.reset();

  std::promise<void> cleanup_complete;
  auto cleanup_future = cleanup_complete.get_future();
  net::post(server_ioc, [&cleanup_complete] { cleanup_complete.set_value(); });
  cleanup_future.wait();

  guard.reset();
  server_thread.join();
  EXPECT_TRUE(detected);
}

TEST(SseClientTest, DeliversEventsFromAnAsyncEventStream)
{
  net::io_context server_ioc;
  tcp::acceptor acceptor(server_ioc, {net::ip::address_v4::loopback(), 0});
  const auto port = acceptor.local_endpoint().port();
  std::atomic<bool> accepted_sse{false};
  std::thread server([&]
  {
    tcp::socket socket(server_ioc);
    acceptor.accept(socket);
    boost::beast::flat_buffer buffer;
    http::request<http::empty_body> request;
    http::read(socket, buffer, request);
    accepted_sse = request[http::field::accept] == "text/event-stream" &&
                   request["Last-Event-ID"] == "41";

    const std::string head =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/event-stream; charset=utf-8\r\n"
      "Connection: close\r\n\r\n";
    net::write(socket, net::buffer(head));
    net::write(socket, net::buffer(std::string("event: con")));
    net::write(socket, net::buffer(std::string("fig\ndata: changed\nid: 42\n\n")));
    boost::system::error_code ignored;
    socket.shutdown(tcp::socket::shutdown_send, ignored);
  });

  net::io_context ioc;
  auto stream = std::make_shared<client::SseClient>(ioc);
  std::vector<sse::SseEvent> events;
  boost::system::error_code closed_with;
  bool closed = false;
  stream->connect(
    "http://127.0.0.1:" + std::to_string(port) + "/events",
    {{"Last-Event-ID", "41"}},
    [&](const sse::SseEvent& event) { events.push_back(event); },
    [&](boost::system::error_code ec) { closed_with = ec; closed = true; });
  ioc.run();
  server.join();

  EXPECT_TRUE(accepted_sse);
  EXPECT_TRUE(closed);
  EXPECT_FALSE(closed_with) << closed_with.message();
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].event, "config");
  EXPECT_EQ(events[0].data, "changed");
  EXPECT_EQ(events[0].id, "42");
}

TEST(SseClientTest, CancelClosesTheStreamAndCompletesExactlyOnce)
{
  net::io_context server_ioc;
  tcp::acceptor acceptor(server_ioc, {net::ip::address_v4::loopback(), 0});
  const auto port = acceptor.local_endpoint().port();
  std::atomic<bool> peer_closed{false};
  std::thread server([&]
  {
    tcp::socket socket(server_ioc);
    acceptor.accept(socket);
    boost::beast::flat_buffer buffer;
    http::request<http::empty_body> request;
    http::read(socket, buffer, request);
    const std::string event = "data: stop\n\n";
    const std::string wire = std::string{
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/event-stream\r\n"
      "Transfer-Encoding: chunked\r\n\r\n"} +
      "c\r\n" + event + "\r\n";
    net::write(socket, net::buffer(wire));
    std::array<char, 1> ignored{};
    boost::system::error_code ec;
    socket.read_some(net::buffer(ignored), ec);
    peer_closed = static_cast<bool>(ec);
  });

  net::io_context ioc;
  auto stream = std::make_shared<client::SseClient>(ioc);
  int close_count = 0;
  boost::system::error_code closed_with;
  stream->connect(
    "http://127.0.0.1:" + std::to_string(port) + "/events", {},
    [stream](const sse::SseEvent&) { stream->cancel(); },
    [&](boost::system::error_code ec) { ++close_count; closed_with = ec; });
  ioc.run();
  server.join();

  EXPECT_TRUE(peer_closed);
  EXPECT_EQ(close_count, 1);
  EXPECT_EQ(closed_with, net::error::operation_aborted);
}

TEST(SseClientTest, RejectsANonEventStreamResponse)
{
  net::io_context server_ioc;
  tcp::acceptor acceptor(server_ioc, {net::ip::address_v4::loopback(), 0});
  const auto port = acceptor.local_endpoint().port();
  std::thread server([&]
  {
    tcp::socket socket(server_ioc);
    acceptor.accept(socket);
    boost::beast::flat_buffer buffer;
    http::request<http::empty_body> request;
    http::read(socket, buffer, request);
    http::response<http::empty_body> response{http::status::ok, 11};
    response.set(http::field::content_type, "application/json");
    response.content_length(0);
    response.keep_alive(false);
    http::write(socket, response);
  });

  net::io_context ioc;
  auto stream = std::make_shared<client::SseClient>(ioc);
  boost::system::error_code closed_with;
  stream->connect(
    "http://127.0.0.1:" + std::to_string(port) + "/events", {},
    [](const sse::SseEvent&) { FAIL() << "non-SSE response emitted an event"; },
    [&](boost::system::error_code ec) { closed_with = ec; });
  ioc.run();
  server.join();

  EXPECT_EQ(closed_with, make_error_code(boost::system::errc::protocol_error));
}

TEST(SseClientTest, ClosesAnEventStreamThatExceedsTheConfiguredLimit)
{
  net::io_context server_ioc;
  tcp::acceptor acceptor(server_ioc, {net::ip::address_v4::loopback(), 0});
  const auto port = acceptor.local_endpoint().port();
  std::thread server([&]
  {
    tcp::socket socket(server_ioc);
    acceptor.accept(socket);
    boost::beast::flat_buffer buffer;
    http::request<http::empty_body> request;
    http::read(socket, buffer, request);
    const std::string wire =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/event-stream\r\n"
      "Connection: close\r\n\r\n"
      "data: " + std::string(64, 'x') + "\n\n";
    boost::system::error_code ignored;
    net::write(socket, net::buffer(wire), ignored);
  });

  net::io_context ioc;
  auto stream = std::make_shared<client::SseClient>(ioc, 32);
  int close_count = 0;
  boost::system::error_code closed_with;
  stream->connect(
    "http://127.0.0.1:" + std::to_string(port) + "/events", {},
    [](const sse::SseEvent&) { FAIL() << "oversized SSE response emitted an event"; },
    [&](boost::system::error_code ec) { ++close_count; closed_with = ec; });
  ioc.run();
  server.join();

  EXPECT_EQ(close_count, 1);
  EXPECT_EQ(closed_with, net::error::message_size);
}

TEST(SseClientTest, ContainsExceptionsThrownByUserCallbacks)
{
  net::io_context server_ioc;
  tcp::acceptor acceptor(server_ioc, {net::ip::address_v4::loopback(), 0});
  const auto port = acceptor.local_endpoint().port();
  std::thread server([&]
  {
    tcp::socket socket(server_ioc);
    acceptor.accept(socket);
    boost::beast::flat_buffer buffer;
    http::request<http::empty_body> request;
    http::read(socket, buffer, request);
    const std::string wire =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/event-stream\r\n"
      "Connection: close\r\n\r\n"
      "data: ready\n\n";
    boost::system::error_code ignored;
    net::write(socket, net::buffer(wire), ignored);
  });

  net::io_context ioc;
  auto stream = std::make_shared<client::SseClient>(ioc);
  int close_count = 0;
  boost::system::error_code closed_with;
  stream->connect(
    "http://127.0.0.1:" + std::to_string(port) + "/events", {},
    [](const sse::SseEvent&) { throw std::runtime_error("application callback failure"); },
    [&](boost::system::error_code ec) { ++close_count; closed_with = ec; });

  EXPECT_NO_THROW(ioc.run());
  server.join();

  EXPECT_EQ(close_count, 1);
  EXPECT_EQ(closed_with, net::error::operation_aborted);
}

TEST(SseClientTest, ContainsExceptionsThrownByCloseCallback)
{
  net::io_context server_ioc;
  tcp::acceptor acceptor(server_ioc, {net::ip::address_v4::loopback(), 0});
  const auto port = acceptor.local_endpoint().port();
  std::thread server([&]
  {
    tcp::socket socket(server_ioc);
    acceptor.accept(socket);
    boost::beast::flat_buffer buffer;
    http::request<http::empty_body> request;
    http::read(socket, buffer, request);
    http::response<http::empty_body> response{http::status::ok, 11};
    response.set(http::field::content_type, "application/json");
    response.content_length(0);
    response.keep_alive(false);
    http::write(socket, response);
  });

  net::io_context ioc;
  auto stream = std::make_shared<client::SseClient>(ioc);
  stream->connect(
    "http://127.0.0.1:" + std::to_string(port) + "/events", {},
    [](const sse::SseEvent&) {},
    [](boost::system::error_code) { throw std::runtime_error("close callback failure"); });

  EXPECT_NO_THROW(ioc.run());
  server.join();
}
