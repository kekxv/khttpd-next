#ifndef KHTTPD_FRAMEWORK_ROUTER_TYPED_ROUTE_HPP_
#define KHTTPD_FRAMEWORK_ROUTER_TYPED_ROUTE_HPP_

#include "router/http_result.hpp"
#include "router/openapi_schema.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cctype>
#include <exception>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <tuple>
#include <utility>

namespace khttpd::framework
{
  // Thrown when a typed route cannot parse a request body before invoking its handler.
  class TypedRequestValidationError final : public std::runtime_error
  {
  public:
    using std::runtime_error::runtime_error;
  };
}

namespace khttpd::framework::detail
{
  struct TypedRouteHandler
  {
    std::function<void(HttpContext&)> handler;
    boost::json::value request_schema;
    std::optional<boost::json::value> response_schema;
  };

  template <class T>
  struct typed_response_body
  {
    using type = T;
  };

  template <class T>
  struct typed_response_body<HttpResult<T>>
  {
    using type = T;
  };

  template <class R, class... Args>
  struct callable_signature
  {
    using return_type = R;
    static constexpr std::size_t arity = sizeof...(Args);

    template <std::size_t Index>
    using argument = std::tuple_element_t<Index, std::tuple<Args...>>;
  };

  template <class T>
  struct callable_traits : callable_traits<decltype(&std::decay_t<T>::operator())> {};

  template <class R, class... Args>
  struct callable_traits<R(Args...)> : callable_signature<R, Args...> {};

  template <class R, class... Args>
  struct callable_traits<R (*)(Args...)> : callable_signature<R, Args...> {};

  template <class R, class... Args>
  struct callable_traits<std::function<R(Args...)>> : callable_signature<R, Args...> {};

  template <class C, class R, class... Args>
  struct callable_traits<R (C::*)(Args...)> : callable_signature<R, Args...> {};

  template <class C, class R, class... Args>
  struct callable_traits<R (C::*)(Args...) const> : callable_signature<R, Args...> {};

  template <class T>
  using remove_cvref_t = std::remove_cv_t<std::remove_reference_t<T>>;

  inline void write_invalid_request_body(HttpContext& context)
  {
    context.set_status(boost::beast::http::status::bad_request);
    boost::json::object error;
    error.emplace("code", "INVALID_REQUEST_BODY");
    error.emplace("message", "Request body must be valid JSON matching the expected schema");
    context.set_body_json(error);
  }

  inline bool is_json_media_type(std::string content_type)
  {
    if (const auto semicolon = content_type.find(';'); semicolon != std::string::npos)
    {
      content_type.resize(semicolon);
    }

    const auto first = std::find_if_not(content_type.begin(), content_type.end(), [](const unsigned char c)
    {
      return std::isspace(c) != 0;
    });
    const auto last = std::find_if_not(content_type.rbegin(), content_type.rend(), [](const unsigned char c)
    {
      return std::isspace(c) != 0;
    }).base();
    if (first >= last) return false;

    const auto media_type = ascii_lower(std::string(first, last));
    constexpr std::string_view prefix = "application/";
    constexpr std::string_view suffix = "+json";
    return media_type == "application/json" ||
      (media_type.size() > prefix.size() + suffix.size() &&
       media_type.compare(0, prefix.size(), prefix) == 0 &&
       media_type.compare(media_type.size() - suffix.size(), suffix.size(), suffix) == 0);
  }

  template <class Handler>
  TypedRouteHandler make_typed_handler(Handler&& input_handler)
  {
    using StoredHandler = std::decay_t<Handler>;
    using Traits = callable_traits<StoredHandler>;
    static_assert(Traits::arity == 1 || Traits::arity == 2,
                  "typed handlers must accept (const Request&) or (const Request&, HttpContext&)");

    using RequestArgument = typename Traits::template argument<0>;
    using Request = remove_cvref_t<RequestArgument>;
    using Response = typename Traits::return_type;

    static_assert(!std::is_same_v<Request, HttpContext>,
                  "legacy HttpContext handlers must use the existing route overload");
    static_assert(!std::is_void_v<Response>,
                  "typed handlers must return a body or HttpResult<void>");
    static_assert(!std::is_reference_v<Response>,
                  "typed handlers must return responses by value");

    if constexpr (Traits::arity == 2)
    {
      using ContextArgument = typename Traits::template argument<1>;
      static_assert(std::is_same_v<ContextArgument, HttpContext&>,
                    "the optional second typed-handler argument must be HttpContext&");
    }

    auto adapted = [handler = StoredHandler(std::forward<Handler>(input_handler))](HttpContext& context) mutable
    {
      const auto content_type = context.get_header(boost::beast::http::field::content_type);
      if (!content_type || !is_json_media_type(*content_type))
      {
        throw TypedRequestValidationError(
          "Request body must be valid JSON matching the expected schema");
      }

      std::optional<boost::json::value> json;
      std::optional<Request> request;
      try
      {
        json.emplace(boost::json::parse(context.body()));
        request.emplace(boost::json::value_to<Request>(*json));
      }
      catch (const std::bad_alloc&)
      {
        throw;
      }
      catch (const std::exception&)
      {
        throw TypedRequestValidationError(
          "Request body must be valid JSON matching the expected schema");
      }

      if constexpr (Traits::arity == 1)
      {
        auto response = std::invoke(handler, static_cast<const Request&>(*request));
        apply_typed_response(context, std::move(response));
      }
      else
      {
        auto response = std::invoke(handler, static_cast<const Request&>(*request), context);
        apply_typed_response(context, std::move(response));
      }
    };

    using ResponseBody = typename typed_response_body<Response>::type;
    std::optional<boost::json::value> response_schema;
    if constexpr (!std::is_void_v<ResponseBody>) response_schema.emplace(openapi_schema<ResponseBody>());
    return {std::move(adapted), openapi_schema<Request>(), std::move(response_schema)};
  }

  template <class Controller, class Response, class Request>
  TypedRouteHandler make_typed_member_handler(
    std::shared_ptr<Controller> controller,
    Response (Controller::*method)(const Request&))
  {
    std::function<Response(const Request&)> bound =
      [controller = std::move(controller), method](const Request& request)
      {
        return std::invoke(method, *controller, request);
      };
    return make_typed_handler(std::move(bound));
  }

  template <class Controller, class Response, class Request>
  TypedRouteHandler make_typed_member_handler(
    std::shared_ptr<Controller> controller,
    Response (Controller::*method)(const Request&) const)
  {
    std::function<Response(const Request&)> bound =
      [controller = std::move(controller), method](const Request& request)
      {
        return std::invoke(method, *controller, request);
      };
    return make_typed_handler(std::move(bound));
  }

  template <class Controller, class Response, class Request>
  TypedRouteHandler make_typed_member_handler(
    std::shared_ptr<Controller> controller,
    Response (Controller::*method)(const Request&, HttpContext&))
  {
    std::function<Response(const Request&, HttpContext&)> bound =
      [controller = std::move(controller), method](const Request& request, HttpContext& context)
      {
        return std::invoke(method, *controller, request, context);
      };
    return make_typed_handler(std::move(bound));
  }

  template <class Controller, class Response, class Request>
  TypedRouteHandler make_typed_member_handler(
    std::shared_ptr<Controller> controller,
    Response (Controller::*method)(const Request&, HttpContext&) const)
  {
    std::function<Response(const Request&, HttpContext&)> bound =
      [controller = std::move(controller), method](const Request& request, HttpContext& context)
      {
        return std::invoke(method, *controller, request, context);
      };
    return make_typed_handler(std::move(bound));
  }
}

#endif // KHTTPD_FRAMEWORK_ROUTER_TYPED_ROUTE_HPP_
