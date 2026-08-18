#ifndef KHTTPD_FRAMEWORK_ROUTER_OPENAPI_HPP_
#define KHTTPD_FRAMEWORK_ROUTER_OPENAPI_HPP_

#include <boost/json.hpp>

#include <string>

namespace khttpd::framework
{
  class HttpRouter;

  struct OpenApiInfo
  {
    std::string title = "khttpd API";
    std::string version = "1.0.0";
  };

  boost::json::object generate_openapi(const HttpRouter& router, const OpenApiInfo& info = {});

  void install_openapi_routes(HttpRouter& router, const OpenApiInfo& info = {},
                              const std::string& spec_path = "/openapi.json",
                              const std::string& docs_path = "/docs",
                              bool enabled = true);

  void export_openapi(const HttpRouter& router, const std::string& output_path,
                      const OpenApiInfo& info = {});
}

#endif // KHTTPD_FRAMEWORK_ROUTER_OPENAPI_HPP_
