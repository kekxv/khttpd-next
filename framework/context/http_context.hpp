#ifndef KHTTPD_FRAMEWORK_CONTEXT_HTTP_CONTEXT_HPP_
#define KHTTPD_FRAMEWORK_CONTEXT_HTTP_CONTEXT_HPP_

#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/url/url_view.hpp>
#include <boost/json.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <string>
#include <map>
#include <optional>
#include <vector>
#include <any>

namespace khttpd::framework
{
  struct MultipartFile
  {
    std::string filename;
    std::string content_type;
    std::string data;
  };

  struct CookieOptions
  {
    int max_age = -1; // -1 means session cookie, 0 means delete
    std::string path = "/";
    std::string domain = "";
    bool secure = false;
    bool http_only = true;
    std::string same_site = "Lax"; // Strict, Lax, None
  };


  class HttpContext
  {
  public:
    using Request = boost::beast::http::request<boost::beast::http::string_body>;
    using Response = boost::beast::http::response<boost::beast::http::string_body>;
    using WriteHandler = std::function<bool(const std::string& buffer)>;
    using HttpStreamHandler = std::function<void(HttpContext&, const WriteHandler&)>;

    HttpContext(Request& req, Response& res,
                std::optional<boost::asio::ip::tcp::endpoint> peer_endpoint = std::nullopt);
    ~HttpContext();

    const std::string& path() const;
    boost::beast::http::verb method() const;
    std::string body() const;
    std::optional<std::string> get_query_param(const std::string& key) const;
    std::optional<std::string> get_path_param(const std::string& key) const;
    std::optional<std::string> get_header(boost::beast::string_view name) const;
    std::optional<std::string> get_header(boost::beast::http::field name) const;
    std::optional<std::vector<std::string>> get_headers(boost::beast::string_view name) const;
    std::optional<std::vector<std::string>> get_headers(boost::beast::http::field name) const;

    // The transport peer, captured from the accepted socket. Unlike forwarded
    // headers this value cannot be supplied by the HTTP client.
    const std::optional<boost::asio::ip::tcp::endpoint>& peer_endpoint() const { return peer_endpoint_; }
    std::optional<boost::asio::ip::address> peer_address() const
    { return peer_endpoint_ ? std::optional{peer_endpoint_->address()} : std::nullopt; }

    // Cookie support
    std::optional<std::string> get_cookie(const std::string& key) const;
    std::vector<std::string> get_cookies(const std::string& key) const;
    void set_cookie(const std::string& key, const std::string& value, const CookieOptions& options = {}) const;

    std::optional<boost::json::value> get_json() const;
    std::optional<std::string> get_form_param(const std::string& key) const;

    std::optional<std::string> get_multipart_field(const std::string& key) const;
    const std::vector<MultipartFile>* get_uploaded_files(const std::string& field_name) const;


    void set_status(boost::beast::http::status status) const;
    void set_body(std::string body) const;

    template <class T>
    void set_body_json(T const& t, boost::json::serialize_options const& opts = {})
    {
      set_content_type("application/json");
      set_body(boost::json::serialize(t, opts));
    }

    template <class T>
    void set_body_from(T const& t, boost::json::storage_ptr sp = {}, boost::json::serialize_options const& opts = {})
    {
      set_body_json(boost::json::value_from(t, sp), opts);
    }

    void chunked(const HttpStreamHandler& handler);
    void set_header(boost::beast::string_view name, boost::beast::string_view value) const;
    void set_header(boost::beast::http::field name, boost::beast::string_view value) const;
    void set_content_type(boost::beast::string_view type) const;

    Request& get_request() const { return req_; }
    Response& get_response() const { return res_; }
    Request& get_request() { return req_; }
    Response& get_response() { return res_; }
    HttpStreamHandler get_stream_handler() const { return do_stream_chunk; }

    void set_path_params(std::map<std::string, std::string> params) const;

    // Extended data for interceptors/handlers
    void set_attribute(const std::string& key, std::any value) const
    {
      extended_data_[key] = std::move(value);
    }

    std::any get_attribute(const std::string& key) const
    {
      auto it = extended_data_.find(key);
      if (it != extended_data_.end())
      {
        return it->second;
      }
      return {};
    }

    template <typename T>
    std::optional<T> get_attribute_as(const std::string& key) const
    {
      auto it = extended_data_.find(key);
      if (it != extended_data_.end())
      {
        try
        {
          return std::any_cast<T>(it->second);
        }
        catch (const std::bad_any_cast&)
        {
          return std::nullopt;
        }
      }
      return std::nullopt;
    }

  private:
    Request& req_;
    Response& res_;
    std::optional<boost::asio::ip::tcp::endpoint> peer_endpoint_;
    mutable std::map<std::string, std::string> query_params_;
    mutable std::string cached_path_;
    mutable boost::urls::url_view parsed_url_;
    mutable bool url_parsed_ = false;

    mutable std::map<std::string, std::string> path_params_;

    mutable std::optional<boost::json::value> cached_json_;
    mutable std::map<std::string, std::string> cached_form_params_;
    mutable bool form_params_parsed_ = false;

    mutable std::map<std::string, std::string> cached_multipart_fields_;
    mutable std::map<std::string, std::vector<MultipartFile>> cached_multipart_files_;
    mutable bool multipart_parsed_ = false;
    HttpStreamHandler do_stream_chunk = nullptr;

    mutable std::map<std::string, std::any> extended_data_;

    mutable std::map<std::string, std::vector<std::string>> cached_cookies_;
    mutable bool cookies_parsed_ = false;
    void parse_cookies() const;

    void parse_url_components() const;
    void parse_form_params() const;
    void parse_multipart_data() const;

    static std::string trim(const std::string& str);
    static std::string extract_header_value(const std::string& headers, const std::string& header_name);
  };
}
#endif // KHTTPD_FRAMEWORK_CONTEXT_HTTP_CONTEXT_HPP_
