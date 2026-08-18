#include "framework/router/http_router.hpp"
#include "framework/router/http_result.hpp"
#include "framework/router/openapi.hpp"

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

#include <memory>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fw = khttpd::framework;
namespace http = boost::beast::http;

struct OpenApiCreatePetRequest
{
  std::string name;
  int age;
  std::optional<std::string> nickname;
};

struct OpenApiPetResponse
{
  int id;
  std::string name;
  std::vector<std::string> tags;
};

BOOST_DESCRIBE_STRUCT(OpenApiCreatePetRequest, (), (name, age, nickname))
BOOST_DESCRIBE_STRUCT(OpenApiPetResponse, (), (id, name, tags))

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace
{
  const boost::json::object& operation_at(const boost::json::object& document,
                                          const std::string& path,
                                          const std::string& method)
  {
    return document.at("paths").as_object().at(path).as_object().at(method).as_object();
  }

  std::string test_output_path(const std::string& filename)
  {
    const auto* directory = std::getenv("TEST_TMPDIR");
    return std::string(directory ? directory : "/tmp") + "/" + filename;
  }

  std::string read_file(const std::string& path)
  {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
  }
}

TEST(OpenApiTest, IncludesLegacyMethodsAndPathParameters)
{
  fw::HttpRouter router;
  router.get("/users/:id", [](fw::HttpContext&) {});
  router.post("/users/:id", [](fw::HttpContext&) {});

  const auto document = fw::generate_openapi(router, {"Accounts API", "9.2.0"});

  EXPECT_EQ(document.at("openapi"), "3.1.0");
  EXPECT_EQ(document.at("info").as_object().at("title"), "Accounts API");
  EXPECT_EQ(document.at("info").as_object().at("version"), "9.2.0");
  const auto& path = document.at("paths").as_object().at("/users/{id}").as_object();
  EXPECT_TRUE(path.contains("get"));
  EXPECT_TRUE(path.contains("post"));

  const auto& parameters = path.at("get").as_object().at("parameters").as_array();
  ASSERT_EQ(parameters.size(), 1U);
  const auto& parameter = parameters.front().as_object();
  EXPECT_EQ(parameter.at("name"), "id");
  EXPECT_EQ(parameter.at("in"), "path");
  EXPECT_EQ(parameter.at("required"), true);
  EXPECT_EQ(parameter.at("schema").as_object().at("type"), "string");
  EXPECT_EQ(parameter.at("x-khttpd-greedy"), true);
}

TEST(OpenApiTest, IncludesTypedDescribeSchemas)
{
  fw::HttpRouter router;
  router.post("/pets", [](const OpenApiCreatePetRequest& request)
  {
    return fw::HttpResult<OpenApiPetResponse>::created({7, request.name, {"new"}});
  });

  const auto document = fw::generate_openapi(router);
  const auto& operation = operation_at(document, "/pets", "post");
  const auto& request_schema = operation.at("requestBody").as_object()
    .at("content").as_object().at("application/json").as_object().at("schema").as_object();
  EXPECT_EQ(request_schema.at("type"), "object");
  const auto& request_properties = request_schema.at("properties").as_object();
  EXPECT_EQ(request_properties.at("name").as_object().at("type"), "string");
  EXPECT_EQ(request_properties.at("age").as_object().at("type"), "integer");
  EXPECT_EQ(request_properties.at("nickname").as_object().at("type"), "string");
  const auto& required = request_schema.at("required").as_array();
  ASSERT_EQ(required.size(), 2U);
  EXPECT_EQ(required[0], "name");
  EXPECT_EQ(required[1], "age");

  const auto& response_schema = operation.at("responses").as_object().at("200").as_object()
    .at("content").as_object().at("application/json").as_object().at("schema").as_object();
  EXPECT_EQ(response_schema.at("type"), "object");
  EXPECT_EQ(response_schema.at("properties").as_object().at("id").as_object().at("type"), "integer");
  const auto& tags = response_schema.at("properties").as_object().at("tags").as_object();
  EXPECT_EQ(tags.at("type"), "array");
  EXPECT_EQ(tags.at("items").as_object().at("type"), "string");
}

TEST(OpenApiTest, DocumentsAsyncAndStreamSkeletons)
{
  fw::HttpRouter router;
  router.async_route("/jobs/:job_id", http::verb::post,
                     [](fw::HttpContext&, fw::HttpAsyncComplete complete) { complete(); });
  router.stream("/downloads/:path", http::verb::get,
                [](fw::HttpContext&, std::shared_ptr<fw::HttpRequestStream>,
                   std::shared_ptr<fw::HttpResponseStream>, fw::HttpStreamComplete complete)
                {
                  complete();
                });

  const auto document = fw::generate_openapi(router);

  EXPECT_TRUE(document.at("paths").as_object().at("/jobs/{job_id}").as_object().contains("post"));
  EXPECT_TRUE(document.at("paths").as_object().at("/downloads/{path}").as_object().contains("get"));
  EXPECT_TRUE(operation_at(document, "/jobs/{job_id}", "post").contains("responses"));
  EXPECT_TRUE(operation_at(document, "/downloads/{path}", "get").contains("responses"));
}

TEST(OpenApiTest, ProducesDeterministicOutput)
{
  fw::HttpRouter router;
  router.get("/z-last", [](fw::HttpContext&) {});
  router.post("/a-first", [](fw::HttpContext&) {});
  router.get("/a-first", [](fw::HttpContext&) {});

  const auto first = boost::json::serialize(fw::generate_openapi(router, {"Stable", "1"}));
  const auto second = boost::json::serialize(fw::generate_openapi(router, {"Stable", "1"}));

  EXPECT_EQ(first, second);
  EXPECT_LT(first.find("/a-first"), first.find("/z-last"));
  EXPECT_LT(first.find("\"get\""), first.find("\"post\""));
}

TEST(OpenApiTest, ServesHiddenRuntimeDocument)
{
  fw::HttpRouter router;
  router.get("/health", [](fw::HttpContext& context)
  {
    context.set_status(http::status::ok);
    context.set_body("ok");
  });
  fw::install_openapi_routes(router, {"Runtime API", "2.0"});

  http::request<http::string_body> request(http::verb::get, "/openapi.json", 11);
  http::response<http::string_body> response;
  fw::HttpContext context(request, response);

  EXPECT_TRUE(router.dispatch(context));
  EXPECT_EQ(response.result(), http::status::ok);
  EXPECT_EQ(response[http::field::content_type], "application/json");
  const auto document = boost::json::parse(response.body()).as_object();
  EXPECT_EQ(document.at("info").as_object().at("title"), "Runtime API");
  EXPECT_TRUE(document.at("paths").as_object().contains("/health"));

  http::request<http::string_body> docs_request(http::verb::get, "/docs", 11);
  http::response<http::string_body> docs_response;
  fw::HttpContext docs_context(docs_request, docs_response);
  EXPECT_TRUE(router.dispatch(docs_context));
  EXPECT_EQ(docs_response.result(), http::status::ok);
  EXPECT_EQ(docs_response[http::field::content_type], "text/html");
  EXPECT_NE(docs_response.body().find("/openapi.json"), std::string::npos);
}

TEST(OpenApiTest, DoesNotDocumentRuntimeDocumentationRoutes)
{
  fw::HttpRouter router;
  router.get("/health", [](fw::HttpContext&) {});
  fw::install_openapi_routes(router);

  const auto paths = fw::generate_openapi(router).at("paths").as_object();

  EXPECT_TRUE(paths.contains("/health"));
  EXPECT_FALSE(paths.contains("/openapi.json"));
  EXPECT_FALSE(paths.contains("/docs"));
}

TEST(OpenApiTest, DoesNotInstallRuntimeDocumentationRoutesWhenDisabled)
{
  fw::HttpRouter router;
  router.get("/health", [](fw::HttpContext&) {});
  fw::install_openapi_routes(router, {}, "/openapi.json", "/docs", false);

  http::request<http::string_body> spec_request(http::verb::get, "/openapi.json", 11);
  http::response<http::string_body> spec_response;
  fw::HttpContext spec_context(spec_request, spec_response);
  EXPECT_FALSE(router.dispatch(spec_context));
  EXPECT_EQ(spec_response.result(), http::status::not_found);

  http::request<http::string_body> docs_request(http::verb::get, "/docs", 11);
  http::response<http::string_body> docs_response;
  fw::HttpContext docs_context(docs_request, docs_response);
  EXPECT_FALSE(router.dispatch(docs_context));
  EXPECT_EQ(docs_response.result(), http::status::not_found);
  EXPECT_TRUE(fw::generate_openapi(router).at("paths").as_object().contains("/health"));
}

TEST(OpenApiTest, ExportsDeterministicJsonFile)
{
  fw::HttpRouter router;
  router.get("/z", [](fw::HttpContext&) {});
  router.get("/a", [](fw::HttpContext&) {});
  const auto first_path = test_output_path("openapi-first.json");
  const auto second_path = test_output_path("openapi-second.json");

  fw::export_openapi(router, first_path, {"Export API", "3"});
  fw::export_openapi(router, second_path, {"Export API", "3"});

  const auto first = read_file(first_path);
  const auto second = read_file(second_path);
  EXPECT_EQ(first, second);
  ASSERT_FALSE(first.empty());
  EXPECT_EQ(first.back(), '\n');
  const auto document = boost::json::parse(first).as_object();
  EXPECT_EQ(document.at("info").as_object().at("version"), "3");
}

TEST(OpenApiTest, RejectsInvalidOutputPath)
{
  fw::HttpRouter router;
  const auto missing_parent = test_output_path("missing-parent/openapi.json");

  EXPECT_THROW(fw::export_openapi(router, ""), std::invalid_argument);
  EXPECT_THROW(fw::export_openapi(router, missing_parent), std::runtime_error);
}

TEST(OpenApiSecurityTest, RejectsConflictingOrDynamicDocumentationPaths)
{
  fw::HttpRouter conflicting_router;
  conflicting_router.get("/openapi.json", [](fw::HttpContext&) {});

  EXPECT_THROW(fw::install_openapi_routes(conflicting_router), std::invalid_argument);

  fw::HttpRouter dynamic_router;
  EXPECT_THROW(fw::install_openapi_routes(dynamic_router, {}, "/spec/:name", "/docs"),
               std::invalid_argument);
  EXPECT_THROW(fw::install_openapi_routes(dynamic_router, {}, "/spec.json", "/docs\r\nInjected"),
               std::invalid_argument);
}
