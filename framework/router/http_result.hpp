#ifndef KHTTPD_FRAMEWORK_ROUTER_HTTP_RESULT_HPP_
#define KHTTPD_FRAMEWORK_ROUTER_HTTP_RESULT_HPP_

#include "context/http_context.hpp"

#include <boost/beast/http/status.hpp>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace khttpd::framework
{
  struct HttpHeader
  {
    std::string name;
    std::string value;
  };

  namespace detail
  {
    inline std::string ascii_lower(std::string value)
    {
      std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c)
      {
        return static_cast<char>(std::tolower(c));
      });
      return value;
    }

    inline bool is_header_token_character(const unsigned char c)
    {
      return std::isalnum(c) != 0 || std::string("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) != std::string::npos;
    }

    inline void validate_response_header(const std::string& name, const std::string& value)
    {
      if (name.empty() || !std::all_of(name.begin(), name.end(), [](const unsigned char c)
      {
        return is_header_token_character(c);
      }))
      {
        throw std::invalid_argument("invalid HTTP response header name");
      }

      if (std::any_of(value.begin(), value.end(), [](const unsigned char c)
      {
        return (c < 0x20 && c != '\t') || c == 0x7f;
      }))
      {
        throw std::invalid_argument("invalid HTTP response header value");
      }

      const auto normalized = ascii_lower(name);
      if (normalized == "content-length" || normalized == "transfer-encoding" ||
          normalized == "connection" || normalized == "keep-alive" ||
          normalized == "proxy-connection" || normalized == "te" ||
          normalized == "trailer" || normalized == "upgrade")
      {
        throw std::invalid_argument("HTTP framing and hop-by-hop headers are managed by khttpd");
      }
    }

    inline void apply_headers(HttpContext& context, const std::vector<HttpHeader>& headers)
    {
      for (const auto& header : headers)
      {
        context.set_header(header.name, header.value);
      }
    }
  }

  template <class T>
  class HttpResult
  {
  public:
    HttpResult(const boost::beast::http::status status, T body)
      : status_(status), body_(std::move(body))
    {
    }

    static HttpResult ok(T body)
    {
      return HttpResult(boost::beast::http::status::ok, std::move(body));
    }

    static HttpResult created(T body)
    {
      return HttpResult(boost::beast::http::status::created, std::move(body));
    }

    HttpResult& header(std::string name, std::string value)
    {
      detail::validate_response_header(name, value);
      headers_.push_back({std::move(name), std::move(value)});
      return *this;
    }

    boost::beast::http::status status() const noexcept { return status_; }
    const T& body() const noexcept { return body_; }
    const std::vector<HttpHeader>& headers() const noexcept { return headers_; }

  private:
    boost::beast::http::status status_;
    T body_;
    std::vector<HttpHeader> headers_;
  };

  template <>
  class HttpResult<void>
  {
  public:
    explicit HttpResult(const boost::beast::http::status status) : status_(status) {}

    static HttpResult no_content()
    {
      return HttpResult(boost::beast::http::status::no_content);
    }

    HttpResult& header(std::string name, std::string value)
    {
      detail::validate_response_header(name, value);
      headers_.push_back({std::move(name), std::move(value)});
      return *this;
    }

    boost::beast::http::status status() const noexcept { return status_; }
    const std::vector<HttpHeader>& headers() const noexcept { return headers_; }

  private:
    boost::beast::http::status status_;
    std::vector<HttpHeader> headers_;
  };

  namespace detail
  {
    template <class T>
    struct is_http_result : std::false_type {};

    template <class T>
    struct is_http_result<HttpResult<T>> : std::true_type {};

    template <class T>
    inline constexpr bool is_http_result_v = is_http_result<std::decay_t<T>>::value;

    template <class T>
    void apply_typed_response(HttpContext& context, const HttpResult<T>& result)
    {
      context.set_status(result.status());
      context.set_body_from(result.body());
      apply_headers(context, result.headers());
    }

    inline void apply_typed_response(HttpContext& context, const HttpResult<void>& result)
    {
      context.set_status(result.status());
      context.set_body("");
      context.get_response().erase(boost::beast::http::field::content_type);
      apply_headers(context, result.headers());
    }

    template <class T, std::enable_if_t<!is_http_result_v<T>, int> = 0>
    void apply_typed_response(HttpContext& context, T&& body)
    {
      context.set_status(boost::beast::http::status::ok);
      context.set_body_from(std::forward<T>(body));
    }
  }
}

#endif // KHTTPD_FRAMEWORK_ROUTER_HTTP_RESULT_HPP_
