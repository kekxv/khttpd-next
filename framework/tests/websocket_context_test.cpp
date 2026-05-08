#include "framework/context/websocket_context.hpp"
#include "framework/websocket/websocket_session.hpp"
#include "framework/router/websocket_router.hpp"
#include <gtest/gtest.h>
#include <boost/asio/io_context.hpp>
#include <memory>

namespace beast = boost::beast;
namespace ws = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;
namespace khttpd_fw = khttpd::framework;

// Minimal mock session for WebsocketContext testing
class SimpleMockWsSession : public khttpd_fw::WebsocketSession
{
public:
  static net::io_context& get_dummy_ioc()
  {
    static net::io_context ioc;
    return ioc;
  }
  static khttpd_fw::WebsocketRouter& get_dummy_router()
  {
    static khttpd_fw::WebsocketRouter router;
    return router;
  }

  SimpleMockWsSession()
    : WebsocketSession(tcp::socket(get_dummy_ioc()), get_dummy_router(), "/mock")
  {
  }

  std::string last_sent;
  bool last_sent_is_text = false;

  void send_message(const std::string& msg, bool is_text) override
  {
    last_sent = msg;
    last_sent_is_text = is_text;
  }
};

class WebsocketContextTest : public ::testing::Test
{
protected:
  std::shared_ptr<SimpleMockWsSession> session;

  void SetUp() override
  {
    session = std::make_shared<SimpleMockWsSession>();
  }
};

TEST_F(WebsocketContextTest, Attributes)
{
  khttpd_fw::WebsocketContext ctx(
    session, "test message", true, "/test");

  ctx.set_attribute("user", std::string("alice"));
  ctx.set_attribute("count", 42);

  auto user = ctx.get_attribute_as<std::string>("user");
  ASSERT_TRUE(user.has_value());
  ASSERT_EQ(user.value(), "alice");

  auto count = ctx.get_attribute_as<int>("count");
  ASSERT_TRUE(count.has_value());
  ASSERT_EQ(count.value(), 42);

  // Missing key
  auto missing = ctx.get_attribute_as<std::string>("missing");
  ASSERT_FALSE(missing.has_value());

  // Type mismatch
  auto wrong = ctx.get_attribute_as<std::string>("count");
  ASSERT_FALSE(wrong.has_value());
}

TEST_F(WebsocketContextTest, SendWithExpiredSession)
{
  khttpd_fw::WebsocketContext ctx(
    session, "hello", true, "/test");

  // Send with valid session
  ctx.send("echo back");
  ASSERT_EQ(session->last_sent, "echo back");

  // Destroy the session
  session.reset();

  // Send with expired session should not crash
  ctx.send("should not crash");
  // last_sent remains unchanged since session is gone
  ASSERT_EQ(ctx.message, "hello"); // Context still has original message
}

TEST_F(WebsocketContextTest, ErrorContext)
{
  beast::error_code test_ec = beast::error::timeout;

  khttpd_fw::WebsocketContext ctx(
    session, "/test", test_ec);

  ASSERT_EQ(ctx.error_code, test_ec);
  ASSERT_EQ(ctx.path, "/test");
  ASSERT_EQ(ctx.is_text, false); // Error context defaults to false
  ASSERT_TRUE(ctx.message.empty()); // Message is default constructed empty
}
