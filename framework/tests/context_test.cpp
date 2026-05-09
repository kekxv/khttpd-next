#include "framework/context/http_context.hpp"
#include <gtest/gtest.h>
#include <boost/beast/http/empty_body.hpp> // For empty body requests

namespace beast = boost::beast;
namespace http = beast::http;
namespace json = boost::json;
namespace khttpd_fw = khttpd::framework;

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
khttpd_fw::HttpContext create_context(
  http::request<http::string_body>& req,
  http::response<http::string_body>& res)
{
  return khttpd_fw::HttpContext(req, res);
}

TEST(HttpContextTest, PathAndMethod)
{
  http::request<http::string_body> req = make_request(http::verb::get, "/users/123?q=test");
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx = create_context(req, res);

  ASSERT_EQ(ctx.path(), "/users/123");
  ASSERT_EQ(ctx.method(), http::verb::get);
}

TEST(HttpContextTest, QueryParameters)
{
  http::request<http::string_body> req = make_request(http::verb::get, "/search?query=boost+beast&page=2");
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx = create_context(req, res);

  ASSERT_TRUE(ctx.get_query_param("query").has_value());
  ASSERT_EQ(ctx.get_query_param("query").value(), "boost beast");

  ASSERT_TRUE(ctx.get_query_param("page").has_value());
  ASSERT_EQ(ctx.get_query_param("page").value(), "2");

  ASSERT_FALSE(ctx.get_query_param("non_existent").has_value());
}

TEST(HttpContextTest, Headers)
{
  http::request<http::string_body> req = make_request(http::verb::get, "/");
  req.set(http::field::user_agent, "Test-Agent/1.0");
  req.set("X-Custom-Header", "CustomValue");
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx = create_context(req, res);

  ASSERT_TRUE(ctx.get_header(http::field::user_agent).has_value());
  ASSERT_EQ(ctx.get_header(http::field::user_agent).value(), "Test-Agent/1.0");

  ASSERT_TRUE(ctx.get_header("X-Custom-Header").has_value());
  ASSERT_EQ(ctx.get_header("X-Custom-Header").value(), "CustomValue");

  ASSERT_FALSE(ctx.get_header("Non-Existent-Header").has_value());
}

TEST(HttpContextTest, JsonBody)
{
  std::string json_str = R"({"name": "Alice", "age": 30})";
  http::request<http::string_body> req = make_request(http::verb::post, "/api/json", 11, json_str);
  req.set(http::field::content_type, "application/json");
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx = create_context(req, res);

  ASSERT_EQ(ctx.body(), json_str);
  ASSERT_TRUE(ctx.get_json().has_value());
  ASSERT_TRUE(ctx.get_json().value().is_object());
  ASSERT_EQ(ctx.get_json().value().at("name").as_string(), "Alice");
  ASSERT_EQ(ctx.get_json().value().at("age").as_int64(), 30);
}

TEST(HttpContextTest, InvalidJsonBody)
{
  std::string invalid_json_str = R"({"name": "Alice", "age": })"; // Malformed JSON
  http::request<http::string_body> req = make_request(http::verb::post, "/api/json", 11, invalid_json_str);
  req.set(http::field::content_type, "application/json");
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx = create_context(req, res);

  ASSERT_FALSE(ctx.get_json().has_value());
}

TEST(HttpContextTest, FormUrlEncodedBody)
{
  std::string form_str = "param1=value1&param2=value%202";
  http::request<http::string_body> req = make_request(http::verb::post, "/api/form", 11, form_str);
  req.set(http::field::content_type, "application/x-www-form-urlencoded");
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx = create_context(req, res);

  ASSERT_TRUE(ctx.get_form_param("param1").has_value());
  ASSERT_EQ(ctx.get_form_param("param1").value(), "value1");
  ASSERT_TRUE(ctx.get_form_param("param2").has_value());
  ASSERT_EQ(ctx.get_form_param("param2").value(), "value 2");
  ASSERT_FALSE(ctx.get_form_param("non_existent").has_value());
}

TEST(HttpContextTest, MultipartFormData)
{
  std::string boundary = "----------WebKitFormBoundary12345";
  std::string multipart_body =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"description\"\r\n\r\n"
    "This is a test description.\r\n"
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"my_test.txt\"\r\n"
    "Content-Type: text/plain\r\n\r\n"
    "Hello, world!\r\n"
    "--" + boundary + "--\r\n";

  http::request<http::string_body> req = make_request(http::verb::post, "/api/upload", 11, multipart_body);
  req.set(http::field::content_type, "multipart/form-data; boundary=" + boundary);
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx = create_context(req, res);

  ASSERT_TRUE(ctx.get_multipart_field("description").has_value());
  ASSERT_EQ(ctx.get_multipart_field("description").value(), "This is a test description.");

  const auto* files = ctx.get_uploaded_files("file");
  ASSERT_NE(files, nullptr);
  ASSERT_EQ(files->size(), 1);
  ASSERT_EQ(files->at(0).filename, "my_test.txt");
  ASSERT_EQ(files->at(0).content_type, "text/plain");
  ASSERT_EQ(files->at(0).data, "Hello, world!");

  ASSERT_EQ(ctx.get_uploaded_files("non_existent_file_field"), nullptr);
  ASSERT_FALSE(ctx.get_multipart_field("non_existent_field").has_value());
}

// Test response setters
TEST(HttpContextTest, ResponseSetters)
{
  http::request<http::string_body> req = make_request(http::verb::get, "/");
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx = create_context(req, res);

  ctx.set_status(http::status::not_found);
  ctx.set_body("Custom 404");
  ctx.set_content_type("text/html");
  ctx.set_header("X-Framework-Version", "1.0");

  // Access the raw response from context to verify
  auto& actual_res = ctx.get_response();

  ASSERT_EQ(actual_res.result(), http::status::not_found);
  ASSERT_EQ(actual_res.body(), "Custom 404");
  ASSERT_EQ(actual_res[http::field::content_type], "text/html");
  ASSERT_EQ(actual_res["X-Framework-Version"], "1.0");
}

TEST(HttpContextTest, Cookies)
{
  http::request<http::string_body> req = make_request(http::verb::get, "/");
  req.set(http::field::cookie, "session_id=12345; user=alice");
  req.insert(http::field::cookie, "theme=dark; user=bob"); // Multiple headers, multiple values for 'user'
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx = create_context(req, res);

  // Single value
  ASSERT_TRUE(ctx.get_cookie("session_id").has_value());
  ASSERT_EQ(ctx.get_cookie("session_id").value(), "12345");
  ASSERT_EQ(ctx.get_cookie("theme").value(), "dark");

  // Multiple values
  auto users = ctx.get_cookies("user");
  ASSERT_EQ(users.size(), 2);
  // Order depends on header order usually, assuming alice first then bob
  ASSERT_EQ(users[0], "alice");
  ASSERT_EQ(users[1], "bob");

  // Missing
  ASSERT_FALSE(ctx.get_cookie("non_existent").has_value());
  ASSERT_TRUE(ctx.get_cookies("non_existent").empty());
}

TEST(HttpContextTest, SetCookie)
{
  http::request<http::string_body> req = make_request(http::verb::get, "/");
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx = create_context(req, res);

  ctx.set_cookie("foo", "bar");
  khttpd_fw::CookieOptions opts;
  opts.max_age = 3600;
  opts.http_only = true;
  opts.secure = true;
  opts.path = "/api";
  opts.domain = "example.com";
  opts.same_site = "Strict";
  ctx.set_cookie("user", "123", opts);

  auto& actual_res = ctx.get_response();
  auto range = actual_res.equal_range(http::field::set_cookie);
  std::vector<std::string> cookies;
  for(auto it = range.first; it != range.second; ++it) {
      cookies.emplace_back(it->value());
  }

  ASSERT_EQ(cookies.size(), 2);
  
  bool found_foo = false;
  bool found_user = false;

  for (const auto& c : cookies) {
      if (c.find("foo=bar") != std::string::npos) {
          found_foo = true;
      }
      if (c.find("user=123") != std::string::npos) {
          found_user = true;
          ASSERT_NE(c.find("Max-Age=3600"), std::string::npos);
          ASSERT_NE(c.find("HttpOnly"), std::string::npos);
          ASSERT_NE(c.find("Secure"), std::string::npos);
          ASSERT_NE(c.find("Path=/api"), std::string::npos);
          ASSERT_NE(c.find("Domain=example.com"), std::string::npos);
          ASSERT_NE(c.find("SameSite=Strict"), std::string::npos);
      }
  }
  ASSERT_TRUE(found_foo);
  ASSERT_TRUE(found_user);
}

// Test set_body_json and set_body_from
TEST(HttpContextTest, SetBodyJson)
{
  http::request<http::string_body> req = make_request(http::verb::get, "/");
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx(req, res);

  boost::json::object obj;
  obj["status"] = "ok";
  obj["count"] = 42;

  ctx.set_body_json(obj);

  auto& actual_res = ctx.get_response();
  ASSERT_EQ(actual_res[http::field::content_type], "application/json");
  ASSERT_EQ(actual_res.body(), R"({"status":"ok","count":42})");
}

TEST(HttpContextTest, SetBodyFrom)
{
  http::request<http::string_body> req = make_request(http::verb::get, "/");
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx(req, res);

  boost::json::object obj;
  obj["name"] = "Bob";
  obj["age"] = 25;

  ctx.set_body_from(boost::json::value_from(obj));

  auto& actual_res = ctx.get_response();
  ASSERT_EQ(actual_res[http::field::content_type], "application/json");
  ASSERT_TRUE(actual_res.body().find("Bob") != std::string::npos);
  ASSERT_TRUE(actual_res.body().find("25") != std::string::npos);
}

// Test path params set/get directly
TEST(HttpContextTest, PathParamsWithoutDispatch)
{
  http::request<http::string_body> req = make_request(http::verb::get, "/");
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx(req, res);

  std::map<std::string, std::string> params;
  params["id"] = "123";
  params["name"] = "alice";
  ctx.set_path_params(params);

  ASSERT_EQ(ctx.get_path_param("id").value(), "123");
  ASSERT_EQ(ctx.get_path_param("name").value(), "alice");
  ASSERT_FALSE(ctx.get_path_param("missing").has_value());
}

// Test context attributes
TEST(HttpContextTest, Attributes)
{
  http::request<http::string_body> req = make_request(http::verb::get, "/");
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx(req, res);

  ctx.set_attribute("user_id", std::string("user-456"));
  ctx.set_attribute("role", 3);

  auto uid = ctx.get_attribute_as<std::string>("user_id");
  ASSERT_TRUE(uid.has_value());
  ASSERT_EQ(uid.value(), "user-456");

  auto role = ctx.get_attribute_as<int>("role");
  ASSERT_TRUE(role.has_value());
  ASSERT_EQ(role.value(), 3);

  // Missing key
  auto missing = ctx.get_attribute_as<std::string>("missing");
  ASSERT_FALSE(missing.has_value());

  // Type mismatch
  auto wrong_type = ctx.get_attribute_as<std::string>("role");
  ASSERT_FALSE(wrong_type.has_value());
}

// Test header case insensitivity
TEST(HttpContextTest, HeaderCaseInsensitive)
{
  http::request<http::string_body> req = make_request(http::verb::get, "/");
  req.set("X-Custom-Header", "value1");
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx(req, res);

  // Boost.Beast stores headers case-insensitively
  ASSERT_TRUE(ctx.get_header("x-custom-header").has_value());
  ASSERT_TRUE(ctx.get_header("X-CUSTOM-HEADER").has_value());
}

// Test set_cookie rejects invalid key characters
TEST(HttpContextTest, InvalidCookieKeyChars)
{
  http::request<http::string_body> req = make_request(http::verb::get, "/");
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx(req, res);

  ctx.set_cookie("bad;key", "value");
  ctx.set_cookie("bad=key", "value");

  auto& actual_res = ctx.get_response();
  auto range = actual_res.equal_range(http::field::set_cookie);
  int count = 0;
  for(auto it = range.first; it != range.second; ++it) { count++; }
  ASSERT_EQ(count, 0); // No cookies should be set
}

// Test set_cookie rejects invalid value characters
TEST(HttpContextTest, InvalidCookieValueChars)
{
  http::request<http::string_body> req = make_request(http::verb::get, "/");
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx(req, res);

  ctx.set_cookie("good_key", "bad;value");
  ctx.set_cookie("good_key2", "bad,value");

  auto& actual_res = ctx.get_response();
  auto range = actual_res.equal_range(http::field::set_cookie);
  int count = 0;
  for(auto it = range.first; it != range.second; ++it) { count++; }
  ASSERT_EQ(count, 0); // No cookies should be set
}

// Test empty path
TEST(HttpContextTest, EmptyPath)
{
  http::request<http::string_body> req = make_request(http::verb::get, "");
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx(req, res);

  ASSERT_EQ(ctx.path(), "");
}

// Test JSON array body
TEST(HttpContextTest, JsonArrayBody)
{
  std::string json_str = R"([1, "two", {"key": "value"}])";
  http::request<http::string_body> req = make_request(http::verb::post, "/api/array", 11, json_str);
  req.set(http::field::content_type, "application/json");
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx(req, res);

  ASSERT_TRUE(ctx.get_json().has_value());
  ASSERT_TRUE(ctx.get_json().value().is_array());
  ASSERT_EQ(ctx.get_json().value().as_array().size(), 3);
}
