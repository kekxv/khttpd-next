#include "router/openapi.hpp"

#include "router/http_router.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace khttpd::framework
{
  namespace
  {
    struct DocumentedPath
    {
      std::string path;
      std::vector<std::string> parameters;
    };

    DocumentedPath document_path(const std::string& route_path)
    {
      static const std::regex parameter_pattern(":([a-zA-Z_][a-zA-Z0-9_]*)");
      DocumentedPath result;
      auto current = route_path.cbegin();
      const std::sregex_iterator end;
      for (auto it = std::sregex_iterator(route_path.cbegin(), route_path.cend(), parameter_pattern);
           it != end; ++it)
      {
        result.path.append(current, it->prefix().second);
        result.path += "{" + (*it)[1].str() + "}";
        result.parameters.push_back((*it)[1].str());
        current = it->suffix().first;
      }
      result.path.append(current, route_path.cend());
      return result;
    }

    std::string method_name(const boost::beast::http::verb method)
    {
      std::string result(boost::beast::http::to_string(method));
      std::transform(result.begin(), result.end(), result.begin(), [](const unsigned char c)
      {
        return static_cast<char>(std::tolower(c));
      });
      return result;
    }

    boost::json::object make_operation(const RouteDescriptor& descriptor,
                                       const std::vector<std::string>& path_parameters)
    {
      boost::json::object operation;
      if (!path_parameters.empty())
      {
        boost::json::array parameters;
        for (std::size_t index = 0; index < path_parameters.size(); ++index)
        {
          boost::json::object parameter;
          parameter.emplace("name", path_parameters[index]);
          parameter.emplace("in", "path");
          parameter.emplace("required", true);
          parameter.emplace("schema", boost::json::object{{"type", "string"}});
          if (index + 1 == path_parameters.size()) parameter.emplace("x-khttpd-greedy", true);
          parameters.emplace_back(std::move(parameter));
        }
        operation.emplace("parameters", std::move(parameters));
      }

      if (descriptor.request_schema)
      {
        boost::json::object media_type;
        media_type.emplace("schema", *descriptor.request_schema);
        boost::json::object content;
        content.emplace("application/json", std::move(media_type));
        boost::json::object request_body;
        request_body.emplace("required", true);
        request_body.emplace("content", std::move(content));
        operation.emplace("requestBody", std::move(request_body));
      }

      boost::json::object response;
      response.emplace("description", "Successful response");
      if (descriptor.response_schema)
      {
        boost::json::object media_type;
        media_type.emplace("schema", *descriptor.response_schema);
        boost::json::object content;
        content.emplace("application/json", std::move(media_type));
        response.emplace("content", std::move(content));
      }
      boost::json::object responses;
      responses.emplace("200", std::move(response));
      operation.emplace("responses", std::move(responses));
      return operation;
    }

    void validate_documentation_path(const std::string& path, const char* name)
    {
      const auto literal_character = [](const unsigned char character)
      {
        return std::isalnum(character) != 0 || character == '/' || character == '.' ||
          character == '_' || character == '-' || character == '~';
      };
      if (path.empty() || path.front() != '/' ||
          !std::all_of(path.begin(), path.end(), literal_character))
        throw std::invalid_argument(std::string(name) + " must be an absolute literal HTTP path");
    }

    std::string escape_html(const std::string& input)
    {
      std::string escaped;
      escaped.reserve(input.size());
      for (const char character : input)
      {
        switch (character)
        {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '\"': escaped += "&quot;"; break;
        case '\'': escaped += "&#39;"; break;
        default: escaped += character; break;
        }
      }
      return escaped;
    }
  }

  boost::json::object generate_openapi(const HttpRouter& router, const OpenApiInfo& info)
  {
    using Operations = std::map<std::string, boost::json::object>;
    std::map<std::string, Operations> documented_paths;

    for (const auto& descriptor : router.route_descriptors())
    {
      auto path = document_path(descriptor.path);
      documented_paths[path.path][method_name(descriptor.method)] =
        make_operation(descriptor, path.parameters);
    }

    boost::json::object paths;
    for (auto& [path, methods] : documented_paths)
    {
      boost::json::object path_item;
      for (auto& [method, operation] : methods)
        path_item.emplace(method, std::move(operation));
      paths.emplace(path, std::move(path_item));
    }

    boost::json::object document;
    document.emplace("openapi", "3.1.0");
    document.emplace("info", boost::json::object{{"title", info.title}, {"version", info.version}});
    document.emplace("paths", std::move(paths));
    return document;
  }

  void install_openapi_routes(HttpRouter& router, const OpenApiInfo& info,
                              const std::string& spec_path, const std::string& docs_path,
                              const bool enabled)
  {
    if (!enabled) return;

    validate_documentation_path(spec_path, "OpenAPI specification path");
    validate_documentation_path(docs_path, "OpenAPI documentation path");
    if (spec_path == docs_path)
      throw std::invalid_argument("OpenAPI specification and documentation paths must differ");

    for (const auto& descriptor : router.route_descriptors())
    {
      if (descriptor.method == boost::beast::http::verb::get &&
          (descriptor.path == spec_path || descriptor.path == docs_path))
        throw std::invalid_argument("OpenAPI documentation path conflicts with an existing GET route: " +
                                    descriptor.path);
    }

    auto serialized_document = std::make_shared<const std::string>(
      boost::json::serialize(generate_openapi(router, info)));
    router.add_route(spec_path, boost::beast::http::verb::get,
      [serialized_document](HttpContext& context)
      {
        context.set_status(boost::beast::http::status::ok);
        context.set_content_type("application/json");
        context.set_body(*serialized_document);
      }, std::nullopt, std::nullopt, false);

    auto page = std::make_shared<const std::string>(
      "<!doctype html><html><head><meta charset=\"utf-8\"><title>khttpd API documentation</title></head>"
      "<body><h1>khttpd API documentation</h1><p><a href=\"" + escape_html(spec_path) +
      "\">OpenAPI 3.1 JSON</a></p></body></html>");
    router.add_route(docs_path, boost::beast::http::verb::get,
      [page](HttpContext& context)
      {
        context.set_status(boost::beast::http::status::ok);
        context.set_content_type("text/html");
        context.set_body(*page);
      }, std::nullopt, std::nullopt, false);
  }

  void export_openapi(const HttpRouter& router, const std::string& output_path,
                      const OpenApiInfo& info)
  {
    if (output_path.empty()) throw std::invalid_argument("OpenAPI output path must not be empty");

    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output.is_open())
      throw std::runtime_error("Unable to open OpenAPI output path: " + output_path);

    output << boost::json::serialize(generate_openapi(router, info)) << '\n';
    output.close();
    if (!output)
      throw std::runtime_error("Unable to write OpenAPI output path: " + output_path);
  }
}
