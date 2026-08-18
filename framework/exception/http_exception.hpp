#ifndef KHTTPD_FRAMEWORK_EXCEPTION_HTTP_EXCEPTION_HPP_
#define KHTTPD_FRAMEWORK_EXCEPTION_HTTP_EXCEPTION_HPP_

#include "exception/exception_handler.hpp"
#include "router/http_result.hpp"

#include <boost/json.hpp>

#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace khttpd::framework
{
  class HttpException : public std::runtime_error
  {
  public:
    template <class Body>
    HttpException(const boost::beast::http::status status,
                  Body&& body,
                  std::string internal_message = "HTTP request failed")
      : std::runtime_error(std::move(internal_message)),
        status_(status),
        body_(boost::json::value_from(std::forward<Body>(body)))
    {
    }

    HttpException& header(std::string name, std::string value)
    {
      detail::validate_response_header(name, value);
      headers_.push_back({std::move(name), std::move(value)});
      return *this;
    }

    boost::beast::http::status status() const noexcept { return status_; }
    const boost::json::value& body() const noexcept { return body_; }
    const std::vector<HttpHeader>& headers() const noexcept { return headers_; }

    void apply(HttpContext& context) const
    {
      context.set_status(status_);
      context.set_body_json(body_);
      detail::apply_headers(context, headers_);
    }

  private:
    boost::beast::http::status status_;
    boost::json::value body_;
    std::vector<HttpHeader> headers_;
  };

  namespace detail
  {
    template <class Exception, class Mapper>
    class TypedExceptionMapper final : public ExceptionHandlerBase
    {
    public:
      explicit TypedExceptionMapper(Mapper mapper) : mapper_(std::move(mapper)) {}

      bool try_handle(std::exception_ptr exception, HttpContext& context) override
      {
        try
        {
          std::rethrow_exception(exception);
        }
        catch (const Exception& value)
        {
          auto response = std::invoke(mapper_, value);
          apply_typed_response(context, std::move(response));
          return true;
        }
        catch (...)
        {
          return false;
        }
      }

    private:
      Mapper mapper_;
    };
  }
}

#endif // KHTTPD_FRAMEWORK_EXCEPTION_HTTP_EXCEPTION_HPP_
