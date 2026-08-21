#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <atomic>
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
