#include "framework/router/http_router.hpp"
#include "framework/router/websocket_router.hpp"
#include "framework/context/http_context.hpp"
#include "framework/context/websocket_context.hpp"
#include "framework/exception/exception_handler.hpp" // Added
#include <gtest/gtest.h>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/core/error.hpp> // For boost::beast::error_code
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "websocket/websocket_session.hpp"

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;
namespace khttpd_fw = khttpd::framework;

// --- HTTP Router Tests (Keeping previous tests for completeness) ---

// Helper function to create a request for testing
template <class Body = http::string_body>
http::request<Body> make_request(
  http::verb method,
  const std::string& target,
  int version = 11,
  const std::string& body_str = "")
{
  http::request<Body> req(method, target, version);
  if (!body_str.empty())
  {
    req.body() = body_str;
    req.prepare_payload();
  }
  return req;
}

// Helper to create HttpContext with dummy response
khttpd_fw::HttpContext create_http_context(
  http::request<http::string_body>& req,
  http::response<http::string_body>& res)
{
  return khttpd_fw::HttpContext(req, res);
}

// 用于捕获处理器调用的标志和数据
struct TestHandlerData
{
  bool called = false;
  std::string path_param_value;
  std::string query_param_value;
  boost::beast::http::status response_status = http::status::continue_;
};

// 重置 TestHandlerData
void reset_handler_data(TestHandlerData& data)
{
  data = TestHandlerData{};
}

TEST(HttpRouterTest, StaticRouteMatching)
{
  khttpd_fw::HttpRouter router;
  TestHandlerData data;

  router.get("/static", [&](khttpd_fw::HttpContext& ctx)
  {
    data.called = true;
    ctx.set_status(http::status::ok);
  });

  http::request<http::string_body> req = make_request(http::verb::get, "/static");
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx = create_http_context(req, res);

  router.dispatch(ctx);

  ASSERT_TRUE(data.called);
  ASSERT_EQ(ctx.get_response().result(), http::status::ok);

  // Test non-existent static route
  reset_handler_data(data);
  req = make_request(http::verb::get, "/nonexistent");
  res = {}; // Reset response
  auto ctx2 = create_http_context(req, res);
  router.dispatch(ctx2);
  ASSERT_FALSE(data.called);
  ASSERT_EQ(ctx2.get_response().result(), http::status::not_found);
}

TEST(HttpRouterTest, DynamicRouteMatchingAndParamExtraction)
{
  khttpd_fw::HttpRouter router;
  TestHandlerData data;

  router.get("/users/:id", [&](khttpd_fw::HttpContext& ctx)
  {
    data.called = true;
    data.path_param_value = ctx.get_path_param("id").value_or("");
    ctx.set_status(http::status::ok);
  });

  http::request<http::string_body> req = make_request(http::verb::get, "/users/456");
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx = create_http_context(req, res);

  router.dispatch(ctx);

  ASSERT_TRUE(data.called);
  ASSERT_EQ(data.path_param_value, "456");
  ASSERT_EQ(ctx.get_response().result(), http::status::ok);

  // Test with query params, ensure path param extraction is still correct
  reset_handler_data(data);
  req = make_request(http::verb::get, "/users/123?name=test");
  res = {};
  auto ctx2 = create_http_context(req, res);
  router.dispatch(ctx2);
  ASSERT_TRUE(data.called);
  ASSERT_EQ(data.path_param_value, "123");
  ASSERT_EQ(ctx2.get_query_param("name").value(), "test"); // Query param still works
}

TEST(HttpRouterTest, RouteSpecificity)
{
  khttpd_fw::HttpRouter router;
  TestHandlerData data;

  // More specific literal route
  router.get("/users/profile", [&](khttpd_fw::HttpContext& ctx)
  {
    data.called = true;
    data.path_param_value = "profile_handler";
    ctx.set_status(http::status::ok);
  });

  // Less specific dynamic route
  router.get("/users/:id", [&](khttpd_fw::HttpContext& ctx)
  {
    data.called = true;
    data.path_param_value = "dynamic_handler:" + ctx.get_path_param("id").value_or("");
    ctx.set_status(http::status::ok);
  });

  // Test matching specific route
  http::request<http::string_body> req_profile = make_request(http::verb::get, "/users/profile");
  http::response<http::string_body> res_profile;
  khttpd_fw::HttpContext ctx_profile = create_http_context(req_profile, res_profile);
  router.dispatch(ctx_profile);
  ASSERT_TRUE(data.called);
  ASSERT_EQ(data.path_param_value, "profile_handler"); // Should match /users/profile

  // Test matching dynamic route
  reset_handler_data(data);
  http::request<http::string_body> req_dynamic = make_request(http::verb::get, "/users/789");
  http::response<http::string_body> res_dynamic;
  khttpd_fw::HttpContext ctx_dynamic = create_http_context(req_dynamic, res_dynamic);
  router.dispatch(ctx_dynamic);
  ASSERT_TRUE(data.called);
  ASSERT_EQ(data.path_param_value, "dynamic_handler:789"); // Should match /users/:id
}

TEST(HttpRouterTest, MultipleDynamicParams)
{
  khttpd_fw::HttpRouter router;
  TestHandlerData data;

  router.get("/items/:category/id/:item_id", [&](khttpd_fw::HttpContext& ctx)
  {
    data.called = true;
    data.path_param_value = ctx.get_path_param("category").value_or("") + ":" + ctx.get_path_param("item_id").
      value_or("");
    ctx.set_status(http::status::ok);
  });

  http::request<http::string_body> req = make_request(http::verb::get, "/items/books/id/12345");
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx = create_http_context(req, res);

  router.dispatch(ctx);

  ASSERT_TRUE(data.called);
  ASSERT_EQ(data.path_param_value, "books:12345");
  ASSERT_EQ(ctx.get_response().result(), http::status::ok);
}

TEST(HttpRouterTest, LastPathParamAllowsSlashes)
{
  khttpd_fw::HttpRouter router;
  TestHandlerData data;

  router.get("/files/:filepath", [&](khttpd_fw::HttpContext& ctx)
  {
    data.called = true;
    data.path_param_value = ctx.get_path_param("filepath").value_or("");
    ctx.set_status(http::status::ok);
  });

  http::request<http::string_body> req = make_request(http::verb::get, "/files/documents/folder/my_report.pdf");
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx = create_http_context(req, res);

  router.dispatch(ctx);

  ASSERT_TRUE(data.called);
  ASSERT_EQ(data.path_param_value, "documents/folder/my_report.pdf");
  ASSERT_EQ(ctx.get_response().result(), http::status::ok);
}


TEST(HttpRouterTest, MethodNotAllowed)
{
  khttpd_fw::HttpRouter router;
  TestHandlerData data;

  router.get("/api/data", []([[maybe_unused]] khttpd_fw::HttpContext& ctx)
  {
    /* empty handler */
  });
  router.post("/api/data", []([[maybe_unused]] khttpd_fw::HttpContext& ctx)
  {
    /* empty handler */
  });

  http::request<http::string_body> req = make_request(http::verb::put, "/api/data");
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx = create_http_context(req, res);

  router.dispatch(ctx);

  ASSERT_EQ(ctx.get_response().result(), http::status::method_not_allowed);
  ASSERT_TRUE(ctx.get_response().find(http::field::allow) != ctx.get_response().end());
  // Order might vary, but should contain GET and POST
  std::string allow_header = std::string(ctx.get_response()[http::field::allow]);
  ASSERT_TRUE(allow_header.find("GET") != std::string::npos);
  ASSERT_TRUE(allow_header.find("POST") != std::string::npos);
  ASSERT_FALSE(allow_header.find("PUT") != std::string::npos);
}

// --- Exception Handling Tests ---

class TestException : public std::runtime_error
{
public:
  TestException(const char* msg) : std::runtime_error(msg)
  {
  }
};

class AnotherException : public std::exception
{
public:
  const char* what() const noexcept override { return "AnotherException"; }
};

TEST(HttpRouterTest, ExceptionHandlerRegistrationAndDispatch)
{
  khttpd_fw::HttpRouter router;
  bool handler_called = false;

  class TestExceptionHandler : public khttpd_fw::ExceptionHandler<TestException>
  {
  public:
    bool* called_ptr;

    TestExceptionHandler(bool* called) : called_ptr(called)
    {
    }

    void handle(const TestException& e, khttpd_fw::HttpContext& ctx) override
    {
      *called_ptr = true;
      ctx.set_status(http::status::bad_request);
      ctx.set_body(e.what());
    }
  };

  router.add_exception_handler(std::make_shared<TestExceptionHandler>(&handler_called));

  http::request<http::string_body> req;
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx(req, res);

  TestException ex("Something went wrong");
  router.handle_exception(std::make_exception_ptr(ex), ctx);

  ASSERT_TRUE(handler_called);
  ASSERT_EQ(ctx.get_response().result(), http::status::bad_request);
  ASSERT_EQ(ctx.get_response().body(), "Something went wrong");
}

TEST(HttpRouterTest, UnhandledExceptionFallback)
{
  khttpd_fw::HttpRouter router;

  http::request<http::string_body> req;
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx(req, res);

  AnotherException ex;
  // Should use default handler (500)
  router.handle_exception(std::make_exception_ptr(ex), ctx);

  ASSERT_EQ(ctx.get_response().result(), http::status::internal_server_error);
}

TEST(HttpRouterTest, ExceptionDispatcherTest)
{
  khttpd_fw::HttpRouter router;
  auto dispatcher = std::make_shared<khttpd_fw::ExceptionDispatcher>();

  bool int_handled = false;
  bool str_handled = false;
  bool runtime_handled = false;

  dispatcher->on<int>([&](const int& e, khttpd_fw::HttpContext& ctx) {
      int_handled = true;
      ctx.set_body(std::to_string(e));
  });

  dispatcher->on<const char*>([&](const char* const& e, khttpd_fw::HttpContext& ctx) {
      str_handled = true;
      ctx.set_body(std::string("String: ") + e);
  });
  
  dispatcher->on<std::runtime_error>([&](const std::runtime_error& e, khttpd_fw::HttpContext& ctx) {
      runtime_handled = true;
      ctx.set_body(std::string("Runtime: ") + e.what());
  });

  router.add_exception_handler(dispatcher);

  // Test int
  {
      http::request<http::string_body> req;
      http::response<http::string_body> res;
      khttpd_fw::HttpContext ctx(req, res);
      router.handle_exception(std::make_exception_ptr(123), ctx);
      ASSERT_TRUE(int_handled);
      ASSERT_EQ(ctx.get_response().body(), "123");
  }

  // Test const char*
  {
      http::request<http::string_body> req;
      http::response<http::string_body> res;
      khttpd_fw::HttpContext ctx(req, res);
      try {
          throw "test_error";
      } catch (...) {
          router.handle_exception(std::current_exception(), ctx);
      }
      ASSERT_TRUE(str_handled);
      ASSERT_EQ(ctx.get_response().body(), "String: test_error");
  }
  
  // Test runtime_error
  {
      http::request<http::string_body> req;
      http::response<http::string_body> res;
      khttpd_fw::HttpContext ctx(req, res);
      router.handle_exception(std::make_exception_ptr(std::runtime_error("oops")), ctx);
      ASSERT_TRUE(runtime_handled);
      ASSERT_EQ(ctx.get_response().body(), "Runtime: oops");
  }
}

TEST(HttpRouterTest, CustomUnknownExceptionHandler)
{
  khttpd_fw::HttpRouter router;
  bool called = false;

  router.set_unknown_exception_handler([&](khttpd_fw::HttpContext& ctx)
  {
    called = true;
    ctx.set_status(http::status::service_unavailable);
  });

  http::request<http::string_body> req;
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx(req, res);

  router.handle_unknown_exception(ctx);

  ASSERT_TRUE(called);
  ASSERT_EQ(ctx.get_response().result(), http::status::service_unavailable);
}

// --- WebSocket Router Tests ---

// Mock WebsocketSession for testing WebsocketContext::send
class MockWebsocketSession : public khttpd_fw::WebsocketSession
{
public:
  std::string last_sent_message;
  bool last_sent_is_text;

  // Static dummy objects required by WebsocketSession base constructor
  static net::io_context dummy_ioc;
  static khttpd_fw::WebsocketRouter dummy_router; // A router instance for the mock

  // Constructor to satisfy base class. We pass a dummy socket and router.
  MockWebsocketSession()
    : khttpd_fw::WebsocketSession(tcp::socket(dummy_ioc), dummy_router, "/mocked_ws_path"), last_sent_is_text(false)
  {
  }

  // Override the base class send_message method to capture data
  void send_message(const std::string& msg, [[maybe_unused]] bool is_text) override
  {
    last_sent_message = msg;
    last_sent_is_text = is_text;
  }
};

// Initialize static members of MockWebsocketSession
net::io_context MockWebsocketSession::dummy_ioc;
khttpd_fw::WebsocketRouter MockWebsocketSession::dummy_router;


// Struct to hold state for WebSocket handlers
struct WsHandlerState
{
  bool on_open_called = false;
  bool on_message_called = false;
  bool on_close_called = false;
  bool on_error_called = false;

  std::string path_received;
  std::string msg_received;
  bool msg_is_text;
  boost::beast::error_code err_code_received;

  // For verifying send() calls within context
  std::shared_ptr<MockWebsocketSession> mock_session_ptr;
};

TEST(WebsocketRouterTest, OpenHandlerDispatch)
{
  khttpd_fw::WebsocketRouter router;
  WsHandlerState state;
  std::string test_path = "/test_open";

  // Setup mock session
  state.mock_session_ptr = std::make_shared<MockWebsocketSession>();
  std::shared_ptr<khttpd_fw::WebsocketSession> base_mock_session =
    std::static_pointer_cast<khttpd_fw::WebsocketSession>(state.mock_session_ptr);

  // Register only on_open handler
  router.add_handler(
    test_path,
    [&](khttpd_fw::WebsocketContext& ctx)
    {
      state.on_open_called = true;
      state.path_received = ctx.path;
      ctx.send("Welcome from test open handler!");
    },
    nullptr, nullptr, nullptr // Other handlers are null
  );

  khttpd_fw::WebsocketContext ctx(base_mock_session, test_path);
  router.dispatch_open(test_path, ctx);

  ASSERT_TRUE(state.on_open_called);
  ASSERT_FALSE(state.on_message_called);
  ASSERT_FALSE(state.on_close_called);
  ASSERT_FALSE(state.on_error_called);
  ASSERT_EQ(state.path_received, test_path);
  ASSERT_EQ(state.mock_session_ptr->last_sent_message, "Welcome from test open handler!");
}

TEST(WebsocketRouterTest, MessageHandlerDispatch)
{
  khttpd_fw::WebsocketRouter router;
  WsHandlerState state;
  std::string test_path = "/test_message";
  std::string sent_msg = "Hello from client!";
  bool is_text = true;

  // Setup mock session
  state.mock_session_ptr = std::make_shared<MockWebsocketSession>();
  std::shared_ptr<khttpd_fw::WebsocketSession> base_mock_session =
    std::static_pointer_cast<khttpd_fw::WebsocketSession>(state.mock_session_ptr);

  // Register only on_message handler
  router.add_handler(
    test_path,
    nullptr, // No open handler
    [&](khttpd_fw::WebsocketContext& ctx)
    {
      state.on_message_called = true;
      state.path_received = ctx.path;
      state.msg_received = ctx.message;
      state.msg_is_text = ctx.is_text;
      ctx.send("Echo: " + ctx.message, ctx.is_text);
    },
    nullptr, nullptr
  );

  khttpd_fw::WebsocketContext ctx(base_mock_session, sent_msg, is_text, test_path);
  router.dispatch_message(test_path, ctx);

  ASSERT_TRUE(state.on_message_called);
  ASSERT_FALSE(state.on_open_called);
  ASSERT_FALSE(state.on_close_called);
  ASSERT_FALSE(state.on_error_called);
  ASSERT_EQ(state.path_received, test_path);
  ASSERT_EQ(state.msg_received, sent_msg);
  ASSERT_EQ(state.msg_is_text, is_text);
  ASSERT_EQ(state.mock_session_ptr->last_sent_message, "Echo: " + sent_msg);
  ASSERT_EQ(state.mock_session_ptr->last_sent_is_text, is_text);
}

TEST(WebsocketRouterTest, CloseHandlerDispatch)
{
  khttpd_fw::WebsocketRouter router;
  WsHandlerState state;
  std::string test_path = "/test_close";

  // Setup mock session
  state.mock_session_ptr = std::make_shared<MockWebsocketSession>();
  std::shared_ptr<khttpd_fw::WebsocketSession> base_mock_session =
    std::static_pointer_cast<khttpd_fw::WebsocketSession>(state.mock_session_ptr);

  // Register only on_close handler
  router.add_handler(
    test_path,
    nullptr, nullptr,
    [&](khttpd_fw::WebsocketContext& ctx)
    {
      state.on_close_called = true;
      state.path_received = ctx.path;
    },
    nullptr
  );

  khttpd_fw::WebsocketContext ctx(base_mock_session, test_path);
  router.dispatch_close(test_path, ctx);

  ASSERT_TRUE(state.on_close_called);
  ASSERT_FALSE(state.on_open_called);
  ASSERT_FALSE(state.on_message_called);
  ASSERT_FALSE(state.on_error_called);
  ASSERT_EQ(state.path_received, test_path);
}

TEST(WebsocketRouterTest, ErrorHandlerDispatch)
{
  khttpd_fw::WebsocketRouter router;
  WsHandlerState state;
  std::string test_path = "/test_error";
  boost::beast::error_code test_error = boost::beast::error::timeout; // Example error

  // Setup mock session
  state.mock_session_ptr = std::make_shared<MockWebsocketSession>();
  std::shared_ptr<khttpd_fw::WebsocketSession> base_mock_session =
    std::static_pointer_cast<khttpd_fw::WebsocketSession>(state.mock_session_ptr);

  // Register only on_error handler
  router.add_handler(
    test_path,
    nullptr, nullptr, nullptr,
    [&](khttpd_fw::WebsocketContext& ctx)
    {
      state.on_error_called = true;
      state.path_received = ctx.path;
      state.err_code_received = ctx.error_code;
    }
  );

  khttpd_fw::WebsocketContext ctx(base_mock_session, test_path, test_error);
  router.dispatch_error(test_path, ctx);

  ASSERT_TRUE(state.on_error_called);
  ASSERT_FALSE(state.on_open_called);
  ASSERT_FALSE(state.on_message_called);
  ASSERT_FALSE(state.on_close_called);
  ASSERT_EQ(state.path_received, test_path);
  ASSERT_EQ(state.err_code_received, test_error);
}

TEST(WebsocketRouterTest, NoHandlerRegistered)
{
  khttpd_fw::WebsocketRouter router; // No handlers registered
  WsHandlerState state;
  std::string test_path = "/unregistered";

  // Setup mock session
  state.mock_session_ptr = std::make_shared<MockWebsocketSession>();
  std::shared_ptr<khttpd_fw::WebsocketSession> base_mock_session =
    std::static_pointer_cast<khttpd_fw::WebsocketSession>(state.mock_session_ptr);

  // Attempt to dispatch all types of events for an unregistered path
  auto tmp = khttpd_fw::WebsocketContext(base_mock_session, "msg", true, test_path);
  router.dispatch_open(test_path, tmp);
  router.dispatch_message(test_path, tmp);
  router.dispatch_close(test_path, tmp);
  auto tmp3 = khttpd_fw::WebsocketContext(base_mock_session, test_path, boost::beast::error::timeout);
  router.dispatch_error(test_path, tmp3);

  ASSERT_FALSE(state.on_open_called);
  ASSERT_FALSE(state.on_message_called);
  ASSERT_FALSE(state.on_close_called);
  ASSERT_FALSE(state.on_error_called);
  // No messages should have been sent by handlers
  ASSERT_TRUE(state.mock_session_ptr->last_sent_message.empty());
}

// Test PUT, DELETE, OPTIONS methods
TEST(HttpRouterTest, PutDelOptionsMethods)
{
  khttpd_fw::HttpRouter router;
  bool put_called = false;
  bool del_called = false;
  bool options_called = false;

  router.put("/resource/:id", [&](khttpd_fw::HttpContext& ctx) {
    put_called = true;
    ctx.set_status(http::status::ok);
  });
  router.del("/resource/:id", [&](khttpd_fw::HttpContext& ctx) {
    del_called = true;
    ctx.set_status(http::status::no_content);
  });
  router.options("/resource", [&](khttpd_fw::HttpContext& ctx) {
    options_called = true;
    ctx.set_status(http::status::ok);
  });

  // Test PUT
  {
    http::request<http::string_body> req = make_request(http::verb::put, "/resource/1");
    http::response<http::string_body> res;
    khttpd_fw::HttpContext ctx(req, res);
    router.dispatch(ctx);
    ASSERT_TRUE(put_called);
    ASSERT_EQ(ctx.get_response().result(), http::status::ok);
  }

  // Test DELETE
  {
    http::request<http::string_body> req = make_request(http::verb::delete_, "/resource/2");
    http::response<http::string_body> res;
    khttpd_fw::HttpContext ctx(req, res);
    router.dispatch(ctx);
    ASSERT_TRUE(del_called);
    ASSERT_EQ(ctx.get_response().result(), http::status::no_content);
  }

  // Test OPTIONS
  {
    http::request<http::string_body> req = make_request(http::verb::options, "/resource");
    http::response<http::string_body> res;
    khttpd_fw::HttpContext ctx(req, res);
    router.dispatch(ctx);
    ASSERT_TRUE(options_called);
    ASSERT_EQ(ctx.get_response().result(), http::status::ok);
  }
}

// Test route update (re-registering same path)
TEST(HttpRouterTest, RouteUpdate)
{
  khttpd_fw::HttpRouter router;
  bool handler_v1_called = false;
  bool handler_v2_called = false;

  router.get("/api/v1", [&](khttpd_fw::HttpContext& ctx) {
    handler_v1_called = true;
    ctx.set_status(http::status::ok);
  });

  // Re-register same path with different handler
  router.get("/api/v1", [&](khttpd_fw::HttpContext& ctx) {
    handler_v2_called = true;
    ctx.set_status(http::status::accepted);
  });

  http::request<http::string_body> req = make_request(http::verb::get, "/api/v1");
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx(req, res);
  router.dispatch(ctx);

  ASSERT_FALSE(handler_v1_called); // Old handler should NOT be called
  ASSERT_TRUE(handler_v2_called);  // New handler should be called
  ASSERT_EQ(ctx.get_response().result(), http::status::accepted);
}

// Test interceptor integration with dispatch
TEST(HttpRouterTest, InterceptorIntegration)
{
  khttpd_fw::HttpRouter router;
  bool handler_called = false;
  bool post_called = false;

  class TestInterceptor : public khttpd_fw::Interceptor {
  public:
    bool* post_ptr;
    TestInterceptor(bool* p) : post_ptr(p) {}
    khttpd_fw::InterceptorResult handle_request(khttpd_fw::HttpContext& ctx) override {
      return khttpd_fw::InterceptorResult::Stop; // Stop processing
    }
    void handle_response(khttpd_fw::HttpContext& ctx) override {
      *post_ptr = true;
      ctx.set_header("X-Post-Interceptor", "yes");
    }
  };

  router.add_interceptor(std::make_shared<TestInterceptor>(&post_called));

  router.get("/stopped", [&](khttpd_fw::HttpContext& ctx) {
    handler_called = true;
    ctx.set_status(http::status::ok);
  });

  http::request<http::string_body> req = make_request(http::verb::get, "/stopped");
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx(req, res);

  // Simulate HttpSession::handle_request interceptor flow
  auto pre_result = router.run_pre_interceptors(ctx);
  if (pre_result == khttpd_fw::InterceptorResult::Stop) {
    router.run_post_interceptors(ctx); // Post-interceptors still run
  } else {
    router.dispatch(ctx);
    router.run_post_interceptors(ctx);
  }

  ASSERT_FALSE(handler_called); // Handler should NOT be called
  ASSERT_TRUE(post_called);     // Post-interceptor should still run
  ASSERT_EQ(ctx.get_response()["X-Post-Interceptor"], "yes");
}

// Test exception dispatcher fallthrough when no handler matches
TEST(HttpRouterTest, ExceptionDispatcherFallthrough)
{
  khttpd_fw::HttpRouter router;
  auto dispatcher = std::make_shared<khttpd_fw::ExceptionDispatcher>();

  bool int_handled = false;
  dispatcher->on<int>([&](const int&, khttpd_fw::HttpContext& ctx) {
    int_handled = true;
    ctx.set_body("int");
  });

  router.add_exception_handler(dispatcher);

  // Throw a type that no handler registered for
  http::request<http::string_body> req;
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx(req, res);

  double d = 3.14;
  router.handle_exception(std::make_exception_ptr(d), ctx);

  ASSERT_FALSE(int_handled); // int handler should NOT be called
  // Default fallback should set 500
  ASSERT_EQ(ctx.get_response().result(), http::status::internal_server_error);
}

// Test wildcard last param with slashes (already tested but add explicit verification)
TEST(HttpRouterTest, WildcardLastParamWithSlashes)
{
  khttpd_fw::HttpRouter router;
  std::string captured;

  router.get("/files/:filepath", [&](khttpd_fw::HttpContext& ctx) {
    captured = ctx.get_path_param("filepath").value_or("");
    ctx.set_status(http::status::ok);
  });

  http::request<http::string_body> req = make_request(http::verb::get, "/files/a/b/c/d.txt");
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx(req, res);
  router.dispatch(ctx);

  ASSERT_TRUE(captured == "a/b/c/d.txt");
  ASSERT_EQ(ctx.get_response().result(), http::status::ok);
}

// Test WebSocket handler overwrite
TEST(WebsocketRouterTest, HandlerOverwrite)
{
  khttpd_fw::WebsocketRouter router;
  WsHandlerState state;
  std::string test_path = "/overwrite_test";

  state.mock_session_ptr = std::make_shared<MockWebsocketSession>();
  std::shared_ptr<khttpd_fw::WebsocketSession> base_mock_session =
    std::static_pointer_cast<khttpd_fw::WebsocketSession>(state.mock_session_ptr);

  // Register first handler
  router.add_handler(
    test_path,
    [&](khttpd_fw::WebsocketContext& ctx) {
      state.on_open_called = true;
      state.path_received = "v1";
    },
    nullptr, nullptr, nullptr
  );

  // Register second handler (should overwrite)
  router.add_handler(
    test_path,
    [&](khttpd_fw::WebsocketContext& ctx) {
      state.on_open_called = true;
      state.path_received = "v2";
    },
    nullptr, nullptr, nullptr
  );

  khttpd_fw::WebsocketContext ctx(base_mock_session, test_path);
  router.dispatch_open(test_path, ctx);

  ASSERT_TRUE(state.on_open_called);
  ASSERT_EQ(state.path_received, "v2"); // Should use the second handler
}

TEST(WebsocketRouterTest, SpecificHandlerRegistered)
{
  khttpd_fw::WebsocketRouter router;
  WsHandlerState state;
  std::string test_path = "/specific";

  // Setup mock session
  state.mock_session_ptr = std::make_shared<MockWebsocketSession>();
  std::shared_ptr<khttpd_fw::WebsocketSession> base_mock_session =
    std::static_pointer_cast<khttpd_fw::WebsocketSession>(state.mock_session_ptr);

  // Register only on_message handler
  router.add_handler(
    test_path,
    nullptr, // No open handler
    [&](khttpd_fw::WebsocketContext& ctx)
    {
      state.on_message_called = true;
      state.msg_received = ctx.message;
    },
    nullptr, nullptr
  );

  // Dispatch a message
  auto tmp = khttpd_fw::WebsocketContext(base_mock_session, "specific message", true, test_path);
  router.dispatch_message(test_path, tmp);
  ASSERT_TRUE(state.on_message_called);
  ASSERT_EQ(state.msg_received, "specific message");

  // Try dispatching other events for the same path
  state.on_message_called = false; // Reset for next check
  auto tmp2 = khttpd_fw::WebsocketContext(base_mock_session, test_path, boost::beast::error::timeout);
  router.dispatch_open(test_path, tmp2);
  router.dispatch_close(test_path, tmp2);
  router.dispatch_error(test_path, tmp2);

  // Ensure only on_message was called, and others were not
  ASSERT_FALSE(state.on_open_called);
  ASSERT_FALSE(state.on_message_called); // Should still be false as no new message dispatch
  ASSERT_FALSE(state.on_close_called);
  ASSERT_FALSE(state.on_error_called);
}
