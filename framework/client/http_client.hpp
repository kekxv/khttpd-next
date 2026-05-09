#ifndef KHTTPD_FRAMEWORK_CLIENT_HTTP_CLIENT_HPP
#define KHTTPD_FRAMEWORK_CLIENT_HTTP_CLIENT_HPP

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/url.hpp>
#include <boost/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <map>
#include <vector>
#include <future>
#include <type_traits>
#include <optional>

#include "client/host_pool.hpp"

namespace khttpd::framework::client
{
  namespace beast = boost::beast;
  namespace http = beast::http;
  namespace net = boost::asio;
  namespace ssl = boost::asio::ssl;
  using tcp = boost::asio::ip::tcp;

  // Helper: String conversion
  template <typename T>
  std::string to_string(const T& val)
  {
    if constexpr (std::is_convertible_v<T, std::string_view>)
    {
      return std::string(val);
    }
    else if constexpr (std::is_arithmetic_v<T>)
    {
      return std::to_string(val);
    }
    else
    {
      return std::to_string(val);
    }
  }

  inline std::string to_string(const std::string& val) { return val; }

  // Helper: Body serialization
  template <typename T>
  std::string serialize_body(const T& value)
  {
    if constexpr (std::is_convertible_v<T, std::string_view>)
    {
      return std::string(value);
    }
    else
    {
      // Assume it's serializable to JSON
      return boost::json::serialize(boost::json::value_from(value));
    }
  }

  // Helper: Replace function
  std::string replace_all(std::string str, const std::string& from, const std::string& to);

  // Verb conversion helper
  inline http::verb verb_from_string(const std::string& s)
  {
    if (s == "GET" || s == "get") return http::verb::get;
    if (s == "POST" || s == "post") return http::verb::post;
    if (s == "PUT" || s == "put") return http::verb::put;
    if (s == "DELETE" || s == "delete") return http::verb::delete_;
    if (s == "PATCH" || s == "patch") return http::verb::patch;
    if (s == "HEAD" || s == "head") return http::verb::head;
    if (s == "OPTIONS" || s == "options") return http::verb::options;
    return http::verb::get;
  }

  class HttpClient : public std::enable_shared_from_this<HttpClient>
  {
  public:
    using ResponseCallback = std::function<void(beast::error_code, http::response<http::string_body>)>;

    // 1. 【新增】傻瓜式构造函数：使用全局 IO 池，内部默认 SSL
    HttpClient();

    // 2. 【新增】使用全局 IO 池，但指定自定义 SSL
    explicit HttpClient(ssl::context& ssl_ctx);

    // 3. 【保留】专家模式：指定外部 IO Context
    explicit HttpClient(net::io_context& ioc);
    HttpClient(net::io_context& ioc, ssl::context& ssl_ctx);

    virtual ~HttpClient() = default;

    // Configuration
    void set_base_url(const std::string& url);
    void set_base_url_pool(const std::vector<HostEntry>& hosts);
    void set_default_header(const std::string& key, const std::string& value);
    void set_bearer_token(const std::string& token);
    void set_timeout(std::chrono::seconds seconds);

    // Core Request Method (Used by Macros)
    void request(http::verb method,
                 std::string path, // relative path or full url
                 const std::map<std::string, std::string>& query_params,
                 const std::string& body,
                 const std::map<std::string, std::string>& headers,
                 ResponseCallback callback);

    // Sync Request Method
    http::response<http::string_body> request_sync(
      http::verb method,
      std::string path,
      const std::map<std::string, std::string>& query_params,
      const std::string& body,
      const std::map<std::string, std::string>& headers);

  private:
    struct UrlParts
    {
      std::string scheme;
      std::string host;
      std::string port;
      std::string target;
    };

    UrlParts parse_target(const std::string& path, const std::map<std::string, std::string>& query);

    net::io_context& ioc_;

    // SSL Context Management
    std::shared_ptr<ssl::context> own_ssl_ctx_; // Holds ownership if created internally
    ssl::context* ssl_ctx_ptr_; // Points to the active context

    std::optional<boost::urls::url> base_url_;
    std::unique_ptr<HostPool> host_pool_; // Multi-host support (null if single host)
    std::map<std::string, std::string> default_headers_;
    std::chrono::seconds timeout_{30};
  };
}

// Include legacy macros (backward compatibility)
#include "macros.hpp"

#endif // KHTTPD_FRAMEWORK_CLIENT_HTTP_CLIENT_HPP
