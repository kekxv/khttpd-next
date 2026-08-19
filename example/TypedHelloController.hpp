#ifndef KHTTPD_EXAMPLE_TYPED_HELLO_CONTROLLER_HPP_
#define KHTTPD_EXAMPLE_TYPED_HELLO_CONTROLLER_HPP_

#include "framework/controller/http_controller.hpp"
#include "framework/router/http_result.hpp"

#include <boost/describe.hpp>

#include <stdexcept>
#include <string>

struct CreateGreetingRequest
{
  std::string name;
};

struct GreetingResponse
{
  std::string message;
};

struct GreetingErrorResponse
{
  std::string code;
  std::string message;
};

BOOST_DESCRIBE_STRUCT(CreateGreetingRequest, (), (name))
BOOST_DESCRIBE_STRUCT(GreetingResponse, (), (message))
BOOST_DESCRIBE_STRUCT(GreetingErrorResponse, (), (code, message))

KHTTPD_OPENAPI_FIELD_DOCUMENTATION(CreateGreetingRequest,
  KHTTPD_OPENAPI_FIELD(name, "Name to include in the greeting."))

KHTTPD_OPENAPI_FIELD_DOCUMENTATION(GreetingResponse,
  KHTTPD_OPENAPI_FIELD(message, "Greeting text returned to the caller."))

KHTTPD_OPENAPI_FIELD_DOCUMENTATION(GreetingErrorResponse,
  KHTTPD_OPENAPI_FIELD(code, "Stable error code.")
  KHTTPD_OPENAPI_FIELD(message, "Error message for the caller."))

class GreetingValidationError : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
};

class TypedHelloController final : public khttpd::framework::BaseController<TypedHelloController>
{
public:
  static std::shared_ptr<TypedHelloController> create()
  {
    return std::make_shared<TypedHelloController>();
  }

  std::shared_ptr<BaseController> register_routes(khttpd::framework::HttpRouter& router) override
  {
    KHTTPD_DOCUMENTED_TYPED_ROUTE(post, "/greetings", create_greeting,
                                  {"Create a greeting",
                                   "Validates a name and returns a created greeting with its Location header."});
    return shared_from_this();
  }

protected:
  std::string base_path() override
  {
    return "/typed";
  }

private:
  khttpd::framework::HttpResult<GreetingResponse> create_greeting(const CreateGreetingRequest& request) const
  {
    if (request.name.empty()) throw GreetingValidationError("name must not be empty");

    auto result = khttpd::framework::HttpResult<GreetingResponse>::created(
      {"Hello, " + request.name + "!"});
    return result.header("Location", "/typed/greetings/latest")
      .header("X-Example-Handler", "typed");
  }
};

#endif // KHTTPD_EXAMPLE_TYPED_HELLO_CONTROLLER_HPP_
