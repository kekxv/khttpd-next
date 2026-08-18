#include "framework/exception/http_exception.hpp"
#include "framework/context/http_context.hpp"
#include "framework/controller/http_controller.hpp"
#include "framework/router/http_result.hpp"
#include "framework/router/http_router.hpp"

#include <boost/beast/http.hpp>
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#pragma clang diagnostic ignored "-Wvariadic-macros"
#pragma clang diagnostic ignored "-Wpedantic"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include <boost/describe.hpp>
#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace http = boost::beast::http;
namespace fw = khttpd::framework;

struct DescribedPayload
{
  int value;
};

BOOST_DESCRIBE_STRUCT(DescribedPayload, (), (value))
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace
{
  struct Reply
  {
    int id;
    std::string name;
  };

  struct CreateRequest
  {
    std::string name;
    int age;
  };

  struct ErrorReply
  {
    std::string code;
    std::string message;
  };

  class ValidationError : public std::runtime_error
  {
  public:
    using std::runtime_error::runtime_error;
  };

  CreateRequest tag_invoke(boost::json::value_to_tag<CreateRequest>, const boost::json::value& value)
  {
    const auto& object = value.as_object();
    return {
      boost::json::value_to<std::string>(object.at("name")),
      boost::json::value_to<int>(object.at("age")),
    };
  }

  void tag_invoke(boost::json::value_from_tag, boost::json::value& value, const ErrorReply& error)
  {
    value = {
      {"code", error.code},
      {"message", error.message},
    };
  }

  void tag_invoke(boost::json::value_from_tag, boost::json::value& value, const Reply& reply)
  {
    value = {
      {"id", reply.id},
      {"name", reply.name},
    };
  }

  fw::HttpContext make_context(http::request<http::string_body>& request,
                               http::response<http::string_body>& response)
  {
    return fw::HttpContext(request, response);
  }

  http::request<http::string_body> json_request(const std::string& target, const std::string& body)
  {
    http::request<http::string_body> request(http::verb::post, target, 11);
    request.set(http::field::content_type, "application/json");
    request.body() = body;
    request.prepare_payload();
    return request;
  }

  class TypedController final : public fw::BaseController<TypedController>
  {
  public:
    std::shared_ptr<BaseController> register_routes(fw::HttpRouter& router) override
    {
      KHTTPD_TYPED_ROUTE(post, "/member", create);
      KHTTPD_TYPED_ROUTE(post, "/const-member", lookup);
      KHTTPD_TYPED_ROUTE(post, "/with-context", with_context);
      return shared_from_this();
    }

  private:
    fw::HttpResult<Reply> create(const CreateRequest& request)
    {
      return fw::HttpResult<Reply>::created(Reply{request.age, request.name});
    }

    Reply lookup(const CreateRequest& request) const
    {
      return Reply{request.age + 1, request.name};
    }

    Reply with_context(const CreateRequest& request, fw::HttpContext& context)
    {
      return Reply{request.age, context.get_header("X-Display-Name").value_or(request.name)};
    }
  };
}

TEST(HttpResultTest, AppliesStatusJsonBodyAndCustomHeaders)
{
  http::request<http::string_body> request;
  http::response<http::string_body> response;
  auto context = make_context(request, response);

  fw::HttpResult<Reply> result(http::status::created, Reply{7, "Ada"});
  result.header("Location", "/users/7").header("X-Request-Id", "req-123");

  fw::detail::apply_typed_response(context, result);

  EXPECT_EQ(response.result(), http::status::created);
  EXPECT_EQ(response[http::field::content_type], "application/json");
  EXPECT_EQ(response[http::field::location], "/users/7");
  EXPECT_EQ(response["X-Request-Id"], "req-123");
  EXPECT_EQ(response.body(), R"({"id":7,"name":"Ada"})");
}

TEST(HttpResultTest, WrapsBareResponseAsJsonOk)
{
  http::request<http::string_body> request;
  http::response<http::string_body> response;
  auto context = make_context(request, response);

  fw::detail::apply_typed_response(context, Reply{8, "Grace"});

  EXPECT_EQ(response.result(), http::status::ok);
  EXPECT_EQ(response[http::field::content_type], "application/json");
  EXPECT_EQ(response.body(), R"({"id":8,"name":"Grace"})");
}

TEST(HttpResultTest, SupportsEmptyNoContentResponse)
{
  http::request<http::string_body> request;
  http::response<http::string_body> response;
  auto context = make_context(request, response);

  auto result = fw::HttpResult<void>::no_content();
  result.header("X-Request-Id", "req-204");
  fw::detail::apply_typed_response(context, result);

  EXPECT_EQ(response.result(), http::status::no_content);
  EXPECT_EQ(response["X-Request-Id"], "req-204");
  EXPECT_TRUE(response.body().empty());
  EXPECT_EQ(response.find(http::field::content_type), response.end());
}

TEST(HttpResultSecurityTest, RejectsHeaderInjectionAndInvalidNames)
{
  fw::HttpResult<Reply> result(http::status::ok, Reply{1, "test"});

  EXPECT_THROW(result.header("X-Test\r\nInjected", "value"), std::invalid_argument);
  EXPECT_THROW(result.header("X Test", "value"), std::invalid_argument);
  EXPECT_THROW(result.header("X-Test", "value\r\nInjected: yes"), std::invalid_argument);
  EXPECT_THROW(result.header("X-Test", std::string("ok\0bad", 6)), std::invalid_argument);
  EXPECT_THROW(result.header("X-Test", std::string("ok\x01", 3)), std::invalid_argument);
  EXPECT_THROW(result.header("X-Test", std::string("ok\x7f", 3)), std::invalid_argument);
}

TEST(HttpResultSecurityTest, RejectsApplicationControlledFramingHeaders)
{
  fw::HttpResult<Reply> result(http::status::ok, Reply{1, "test"});

  EXPECT_THROW(result.header("Content-Length", "1"), std::invalid_argument);
  EXPECT_THROW(result.header("content-length", "1"), std::invalid_argument);
  EXPECT_THROW(result.header("Transfer-Encoding", "chunked"), std::invalid_argument);
  EXPECT_THROW(result.header("Connection", "keep-alive"), std::invalid_argument);
  EXPECT_THROW(result.header("Keep-Alive", "timeout=10"), std::invalid_argument);
  EXPECT_THROW(result.header("Upgrade", "websocket"), std::invalid_argument);
  EXPECT_THROW(result.header("Trailer", "X-Checksum"), std::invalid_argument);
}

TEST(HttpResultSecurityTest, AcceptsOrdinaryResponseHeaders)
{
  fw::HttpResult<Reply> result(http::status::ok, Reply{1, "test"});

  EXPECT_NO_THROW(result.header("Location", "/safe").header("X-Correlation-Id", "abc-123"));
  ASSERT_EQ(result.headers().size(), 2U);
  EXPECT_EQ(result.headers()[0].name, "Location");
  EXPECT_EQ(result.headers()[1].value, "abc-123");
}

TEST(TypedRouteTest, ConvertsJsonAndAppliesHttpResult)
{
  fw::HttpRouter router;
  int calls = 0;
  router.post("/users", [&calls](const CreateRequest& request)
  {
    ++calls;
    auto result = fw::HttpResult<Reply>::created(Reply{request.age, request.name});
    return result.header("Location", "/users/42");
  });

  auto request = json_request("/users", R"({"name":"Ada","age":42})");
  http::response<http::string_body> response;
  auto context = make_context(request, response);

  EXPECT_TRUE(router.dispatch(context));
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(response.result(), http::status::created);
  EXPECT_EQ(response[http::field::location], "/users/42");
  EXPECT_EQ(response.body(), R"({"id":42,"name":"Ada"})");
}

TEST(TypedRouteTest, InvalidBodiesReturnStableBadRequestWithoutCallingHandler)
{
  const std::string expected =
    R"({"code":"INVALID_REQUEST_BODY","message":"Request body must be valid JSON matching the expected schema"})";

  struct Case
  {
    const char* body;
    bool json_content_type;
  };
  const Case cases[] = {
    {R"({"name":"Ada","age":42})", false},
    {R"({"name":)", true},
    {R"({"name":"Ada","age":"old"})", true},
  };

  for (const auto& test_case : cases)
  {
    fw::HttpRouter router;
    int calls = 0;
    router.post("/users", [&calls](const CreateRequest& request)
    {
      ++calls;
      return Reply{request.age, request.name};
    });

    http::request<http::string_body> request(http::verb::post, "/users", 11);
    if (test_case.json_content_type) request.set(http::field::content_type, "application/json");
    request.body() = test_case.body;
    request.prepare_payload();
    http::response<http::string_body> response;
    auto context = make_context(request, response);

    EXPECT_TRUE(router.dispatch(context));
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(response.result(), http::status::bad_request);
    EXPECT_EQ(response[http::field::content_type], "application/json");
    EXPECT_EQ(response.body(), expected);
  }
}

TEST(TypedRouteSecurityTest, RejectsMisleadingJsonMediaType)
{
  fw::HttpRouter router;
  int calls = 0;
  router.post("/users", [&calls](const CreateRequest& request)
  {
    ++calls;
    return Reply{request.age, request.name};
  });

  auto request = json_request("/users", R"({"name":"Ada","age":42})");
  request.set(http::field::content_type, "application/json.evil");
  http::response<http::string_body> response;
  auto context = make_context(request, response);

  EXPECT_TRUE(router.dispatch(context));
  EXPECT_EQ(calls, 0);
  EXPECT_EQ(response.result(), http::status::bad_request);
  EXPECT_EQ(response.body(),
            R"({"code":"INVALID_REQUEST_BODY","message":"Request body must be valid JSON matching the expected schema"})");
}

TEST(TypedRouteTest, AcceptsJsonMediaTypeParameters)
{
  fw::HttpRouter router;
  router.post("/users", [](const CreateRequest& request)
  {
    return Reply{request.age, request.name};
  });

  auto request = json_request("/users", R"({"name":"Ada","age":42})");
  request.set(http::field::content_type, "Application/JSON; charset=utf-8");
  http::response<http::string_body> response;
  auto context = make_context(request, response);

  EXPECT_TRUE(router.dispatch(context));
  EXPECT_EQ(response.result(), http::status::ok);
  EXPECT_EQ(response.body(), R"({"id":42,"name":"Ada"})");
}

TEST(TypedRouteTest, SupportsBoostDescribeDtoWithoutCustomJsonConverters)
{
  fw::HttpRouter router;
  router.post("/described", [](const DescribedPayload& request)
  {
    return DescribedPayload{request.value + 1};
  });

  auto request = json_request("/described", R"({"value":41})");
  http::response<http::string_body> response;
  auto context = make_context(request, response);

  EXPECT_TRUE(router.dispatch(context));
  EXPECT_EQ(response.result(), http::status::ok);
  EXPECT_EQ(response.body(), R"({"value":42})");
}

TEST(TypedRouteTest, SupportsStdFunctionAndEveryBufferedVerb)
{
  fw::HttpRouter router;
  std::function<Reply(const CreateRequest&)> get_handler = [](const CreateRequest& request)
  {
    return Reply{request.age, "get:" + request.name};
  };
  router.get("/typed-get", std::move(get_handler));
  router.post("/typed-post", [](const CreateRequest& request) { return Reply{request.age, "post:" + request.name}; });
  router.put("/typed-put", [](const CreateRequest& request) { return Reply{request.age, "put:" + request.name}; });
  router.del("/typed-delete", [](const CreateRequest& request) { return Reply{request.age, "delete:" + request.name}; });
  router.options("/typed-options", [](const CreateRequest& request) { return Reply{request.age, "options:" + request.name}; });

  struct Case
  {
    http::verb verb;
    const char* path;
    const char* expected_name;
  };
  const Case cases[] = {
    {http::verb::get, "/typed-get", "get:Ada"},
    {http::verb::post, "/typed-post", "post:Ada"},
    {http::verb::put, "/typed-put", "put:Ada"},
    {http::verb::delete_, "/typed-delete", "delete:Ada"},
    {http::verb::options, "/typed-options", "options:Ada"},
  };

  for (const auto& test_case : cases)
  {
    auto request = json_request(test_case.path, R"({"name":"Ada","age":42})");
    request.method(test_case.verb);
    http::response<http::string_body> response;
    auto context = make_context(request, response);
    EXPECT_TRUE(router.dispatch(context));
    EXPECT_EQ(response.result(), http::status::ok);
    EXPECT_EQ(response.body(),
              std::string(R"({"id":42,"name":")") + test_case.expected_name + R"("})");
  }
}

TEST(TypedRouteTest, SupportsControllerMembersConstMembersAndContextInjection)
{
  fw::HttpRouter router;
  auto controller = std::make_shared<TypedController>();
  std::weak_ptr<TypedController> lifetime = controller;
  controller->register_routes(router);
  controller.reset();
  ASSERT_FALSE(lifetime.expired());

  {
    auto request = json_request("/member", R"({"name":"Ada","age":10})");
    http::response<http::string_body> response;
    auto context = make_context(request, response);
    EXPECT_TRUE(router.dispatch(context));
    EXPECT_EQ(response.result(), http::status::created);
    EXPECT_EQ(response.body(), R"({"id":10,"name":"Ada"})");
  }

  {
    auto request = json_request("/const-member", R"({"name":"Grace","age":10})");
    http::response<http::string_body> response;
    auto context = make_context(request, response);
    EXPECT_TRUE(router.dispatch(context));
    EXPECT_EQ(response.result(), http::status::ok);
    EXPECT_EQ(response.body(), R"({"id":11,"name":"Grace"})");
  }

  {
    auto request = json_request("/with-context", R"({"name":"fallback","age":12})");
    request.set("X-Display-Name", "Header Name");
    http::response<http::string_body> response;
    auto context = make_context(request, response);
    EXPECT_TRUE(router.dispatch(context));
    EXPECT_EQ(response.body(), R"({"id":12,"name":"Header Name"})");
  }
}

TEST(TypedRouteCompatibilityTest, LegacyHttpContextHandlerRemainsUnchanged)
{
  fw::HttpRouter router;
  router.post("/legacy", [](fw::HttpContext& context)
  {
    context.set_status(http::status::accepted);
    context.set_content_type("text/plain");
    context.set_body("legacy");
  });

  http::request<http::string_body> request(http::verb::post, "/legacy", 11);
  http::response<http::string_body> response;
  auto context = make_context(request, response);

  EXPECT_TRUE(router.dispatch(context));
  EXPECT_EQ(response.result(), http::status::accepted);
  EXPECT_EQ(response[http::field::content_type], "text/plain");
  EXPECT_EQ(response.body(), "legacy");
}

TEST(TypedExceptionTest, MapsRegisteredExceptionToTypedResponse)
{
  fw::HttpRouter router;
  int mapper_calls = 0;
  router.map_exception<ValidationError>([&mapper_calls](const ValidationError& error)
  {
    ++mapper_calls;
    fw::HttpResult<ErrorReply> result(
      http::status::unprocessable_entity,
      ErrorReply{"VALIDATION_FAILED", error.what()});
    return result.header("X-Error-Source", "validation");
  });

  http::request<http::string_body> request;
  http::response<http::string_body> response;
  auto context = make_context(request, response);
  router.handle_exception(std::make_exception_ptr(ValidationError("age is invalid")), context);

  EXPECT_EQ(mapper_calls, 1);
  EXPECT_EQ(response.result(), http::status::unprocessable_entity);
  EXPECT_EQ(response["X-Error-Source"], "validation");
  EXPECT_EQ(response.body(), R"({"code":"VALIDATION_FAILED","message":"age is invalid"})");
}

TEST(TypedExceptionTest, SerializesHttpExceptionBodyStatusAndHeaders)
{
  fw::HttpRouter router;
  fw::HttpException exception(
    http::status::conflict,
    ErrorReply{"VERSION_CONFLICT", "resource was updated"},
    "optimistic lock conflict for internal record 99");
  exception.header("Retry-After", "1");

  http::request<http::string_body> request;
  http::response<http::string_body> response;
  auto context = make_context(request, response);
  router.handle_exception(std::make_exception_ptr(exception), context);

  EXPECT_EQ(response.result(), http::status::conflict);
  EXPECT_EQ(response[http::field::retry_after], "1");
  EXPECT_EQ(response.body(), R"({"code":"VERSION_CONFLICT","message":"resource was updated"})");
  EXPECT_EQ(response.body().find("internal record 99"), std::string::npos);
}

TEST(TypedExceptionSecurityTest, UnknownStdExceptionDoesNotLeakDetails)
{
  fw::HttpRouter router;
  http::request<http::string_body> request;
  http::response<http::string_body> response;
  auto context = make_context(request, response);

  router.handle_exception(
    std::make_exception_ptr(std::runtime_error("database password=secret at 10.0.0.4")), context);

  EXPECT_EQ(response.result(), http::status::internal_server_error);
  EXPECT_EQ(response[http::field::content_type], "application/json");
  EXPECT_EQ(response.body(), R"({"code":"INTERNAL_SERVER_ERROR","message":"Internal server error"})");
  EXPECT_EQ(response.body().find("database"), std::string::npos);
  EXPECT_EQ(response.body().find("password"), std::string::npos);
  EXPECT_EQ(response.body().find("secret"), std::string::npos);
}

TEST(TypedExceptionSecurityTest, ExceptionResponseClearsPartiallyWrittenSensitiveState)
{
  fw::HttpRouter router;
  http::request<http::string_body> request;
  http::response<http::string_body> response;
  auto context = make_context(request, response);
  context.set_status(http::status::ok);
  context.set_header("X-Internal-Secret", "sensitive");
  context.set_body("partial secret response");

  router.handle_exception(std::make_exception_ptr(std::runtime_error("failed")), context);

  EXPECT_EQ(response.result(), http::status::internal_server_error);
  EXPECT_EQ(response.find("X-Internal-Secret"), response.end());
  EXPECT_EQ(response.body(), R"({"code":"INTERNAL_SERVER_ERROR","message":"Internal server error"})");
}

TEST(TypedExceptionSecurityTest, NullExceptionPointerAlsoClearsPartialResponse)
{
  fw::HttpRouter router;
  http::request<http::string_body> request;
  http::response<http::string_body> response;
  auto context = make_context(request, response);
  context.set_header("X-Internal-Secret", "sensitive");
  context.set_body("partial secret response");

  router.handle_exception(nullptr, context);

  EXPECT_EQ(response.result(), http::status::internal_server_error);
  EXPECT_EQ(response.find("X-Internal-Secret"), response.end());
  EXPECT_EQ(response.body(), R"({"code":"INTERNAL_SERVER_ERROR","message":"Internal server error"})");
}

TEST(TypedExceptionSecurityTest, MapperFailureFallsBackToSafeInternalServerError)
{
  fw::HttpRouter router;
  router.map_exception<ValidationError>([](const ValidationError&) -> ErrorReply
  {
    throw std::runtime_error("mapper secret");
  });

  http::request<http::string_body> request;
  http::response<http::string_body> response;
  auto context = make_context(request, response);

  EXPECT_NO_THROW(router.handle_exception(std::make_exception_ptr(ValidationError("invalid")), context));
  EXPECT_EQ(response.result(), http::status::internal_server_error);
  EXPECT_EQ(response.body(), R"({"code":"INTERNAL_SERVER_ERROR","message":"Internal server error"})");
  EXPECT_EQ(response.body().find("mapper secret"), std::string::npos);
}

TEST(TypedExceptionSecurityTest, HttpExceptionRejectsInjectedHeaders)
{
  fw::HttpException exception(
    http::status::bad_request,
    ErrorReply{"BAD_REQUEST", "invalid"});

  EXPECT_THROW(exception.header("X-Test", "safe\r\nInjected: yes"), std::invalid_argument);
  EXPECT_THROW(exception.header("Content-Length", "999"), std::invalid_argument);
}
