#include "framework/context/http_context.hpp"
#include <gtest/gtest.h>

namespace beast = boost::beast;
namespace http = beast::http;
namespace khttpd_fw = khttpd::framework;

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

class MultipartEdgeTest : public ::testing::Test
{
};

// Test multiple files uploaded under the same form field name
TEST_F(MultipartEdgeTest, MultipleFilesInSameField)
{
  std::string boundary = "----------Boundary123";
  std::string multipart_body =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"files\"; filename=\"photo1.jpg\"\r\n"
    "Content-Type: image/jpeg\r\n\r\n"
    "IMAGE_DATA_1\r\n"
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"files\"; filename=\"photo2.png\"\r\n"
    "Content-Type: image/png\r\n\r\n"
    "IMAGE_DATA_2\r\n"
    "--" + boundary + "--\r\n";

  http::request<http::string_body> req = make_request(http::verb::post, "/upload", 11, multipart_body);
  req.set(http::field::content_type, "multipart/form-data; boundary=" + boundary);
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx(req, res);

  const auto* files = ctx.get_uploaded_files("files");
  ASSERT_NE(files, nullptr);
  ASSERT_EQ(files->size(), 2);

  ASSERT_EQ(files->at(0).filename, "photo1.jpg");
  ASSERT_EQ(files->at(0).content_type, "image/jpeg");
  ASSERT_EQ(files->at(0).data, "IMAGE_DATA_1");

  ASSERT_EQ(files->at(1).filename, "photo2.png");
  ASSERT_EQ(files->at(1).content_type, "image/png");
  ASSERT_EQ(files->at(1).data, "IMAGE_DATA_2");
}

// Test that same form field name results in last value winning (overwrite)
TEST_F(MultipartEdgeTest, FormFieldOverwrite)
{
  std::string boundary = "----------Boundary456";
  std::string multipart_body =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"username\"\r\n\r\n"
    "first_value\r\n"
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"username\"\r\n\r\n"
    "second_value\r\n"
    "--" + boundary + "--\r\n";

  http::request<http::string_body> req = make_request(http::verb::post, "/form", 11, multipart_body);
  req.set(http::field::content_type, "multipart/form-data; boundary=" + boundary);
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx(req, res);

  // Last value should win (overwrite behavior)
  ASSERT_TRUE(ctx.get_multipart_field("username").has_value());
  ASSERT_EQ(ctx.get_multipart_field("username").value(), "second_value");
}

// Test multipart body without proper boundary - should not crash
TEST_F(MultipartEdgeTest, MissingBoundary)
{
  // Body that claims to be multipart but has no proper boundary markers
  std::string boundary = "----------NonExistent";
  std::string body = "This is not a valid multipart body at all";

  http::request<http::string_body> req = make_request(http::verb::post, "/upload", 11, body);
  req.set(http::field::content_type, "multipart/form-data; boundary=" + boundary);
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx(req, res);

  // Should return nullptr/empty results, not crash
  const auto* files = ctx.get_uploaded_files("any_field");
  ASSERT_EQ(files, nullptr);

  ASSERT_FALSE(ctx.get_multipart_field("any_field").has_value());
}

TEST_F(MultipartEdgeTest, QuotedBoundary)
{
  std::string boundary = "----QuotedBoundary";
  std::string multipart_body =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"field\"\r\n\r\n"
    "quoted boundary works\r\n"
    "--" + boundary + "--\r\n";

  http::request<http::string_body> req = make_request(http::verb::post, "/form", 11, multipart_body);
  req.set(http::field::content_type, "multipart/form-data; boundary=\"" + boundary + "\"");
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx(req, res);

  ASSERT_TRUE(ctx.get_multipart_field("field").has_value());
  ASSERT_EQ(ctx.get_multipart_field("field").value(), "quoted boundary works");
}

TEST_F(MultipartEdgeTest, BoundaryTextInsideFileDataIsPreserved)
{
  std::string boundary = "----BoundaryInData";
  std::string file_data = "line one --" + boundary + " is not a delimiter";
  std::string multipart_body =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"data.txt\"\r\n"
    "Content-Type: text/plain\r\n\r\n" +
    file_data + "\r\n"
    "--" + boundary + "--\r\n";

  http::request<http::string_body> req = make_request(http::verb::post, "/upload", 11, multipart_body);
  req.set(http::field::content_type, "multipart/form-data; boundary=" + boundary);
  http::response<http::string_body> res;
  khttpd_fw::HttpContext ctx(req, res);

  const auto* files = ctx.get_uploaded_files("file");
  ASSERT_NE(files, nullptr);
  ASSERT_EQ(files->size(), 1);
  ASSERT_EQ(files->at(0).data, file_data);
}
