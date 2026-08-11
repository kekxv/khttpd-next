#include "http_session_test_harness.hpp"
#include "framework/server.hpp"

#include <gtest/gtest.h>

namespace fw = khttpd::framework;
namespace test = khttpd::framework::tests;
namespace http = boost::beast::http;

namespace
{
  http::response<http::string_body> send_body(std::uint64_t limit, std::string body,
                                               bool chunked, bool& handled)
  {
    test::TempWebRoot web_root;
    fw::HttpRouter router;
    fw::WebsocketRouter websocket_router;
    router.post("/body", [&handled](fw::HttpContext& ctx)
    {
      handled = true;
      ctx.set_body(std::to_string(ctx.get_request().body().size()));
    });
    http::request<http::string_body> request{http::verb::post, "/body", 11};
    request.body() = std::move(body);
    if (chunked) request.chunked(true); else request.prepare_payload();
    request.keep_alive(false);
    return test::round_trip(router, websocket_router, web_root.path, std::move(request), limit);
  }
}

TEST(BufferedBodyLimitTest, DefaultLimitRemainsSixteenMiB)
{
  EXPECT_EQ(fw::HttpSession::default_max_buffered_request_body_size, 16ULL * 1024 * 1024);
  EXPECT_EQ(fw::Server::default_max_buffered_request_body_size, 16ULL * 1024 * 1024);
}

TEST(BufferedBodyLimitTest, ZeroLimitAcceptsEmptyBody)
{
  bool handled = false;
  const auto response = send_body(0, {}, false, handled);
  EXPECT_EQ(response.result(), http::status::ok);
  EXPECT_TRUE(handled);
  EXPECT_EQ(response.body(), "0");
}

TEST(BufferedBodyLimitTest, ZeroLimitRejectsOneByte)
{
  bool handled = false;
  const auto response = send_body(0, "x", false, handled);
  EXPECT_EQ(response.result(), http::status::payload_too_large);
  EXPECT_FALSE(handled);
  EXPECT_FALSE(response.keep_alive());
}

TEST(BufferedBodyLimitTest, ContentLengthAtLimitIsAccepted)
{
  bool handled = false;
  const auto response = send_body(17, std::string(17, 'x'), false, handled);
  EXPECT_EQ(response.result(), http::status::ok);
  EXPECT_TRUE(handled);
  EXPECT_EQ(response.body(), "17");
}

TEST(BufferedBodyLimitTest, ChunkedBodyIsRejectedAfterCumulativeLimit)
{
  bool handled = false;
  const auto response = send_body(31, std::string(32, 'x'), true, handled);
  EXPECT_EQ(response.result(), http::status::payload_too_large);
  EXPECT_FALSE(handled);
}
