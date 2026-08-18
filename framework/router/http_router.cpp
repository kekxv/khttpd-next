// framework/router/http_router.cpp
#include "http_router.hpp"
#include <fmt/core.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <boost/beast/version.hpp>
#include <stdexcept>

namespace khttpd::framework
{
  namespace
  {
    void write_internal_server_error(HttpContext& ctx)
    {
      ctx.set_status(boost::beast::http::status::internal_server_error);
      boost::json::object error;
      error.emplace("code", "INTERNAL_SERVER_ERROR");
      error.emplace("message", "Internal server error");
      ctx.set_body_json(error);
    }

    void reset_exception_response(HttpContext& ctx)
    {
      auto& response = ctx.get_response();
      response = {};
      response.version(ctx.get_request().version());
      response.keep_alive(ctx.get_request().keep_alive());
      response.set(boost::beast::http::field::server, BOOST_BEAST_VERSION_STRING);
    }
  }

  HttpRouter::HttpRouter() = default;

  std::tuple<std::regex, std::vector<std::string>, int, int> HttpRouter::parse_path_pattern(
    const std::string& path_pattern)
  {
    std::string regex_str = "^";
    std::vector<std::string> param_names;
    std::regex param_regex(":([a-zA-Z_][a-zA-Z0-9_]*)"); // search :paramName

    int literal_segments = 0;
    int dynamic_segments = 0;

    std::regex escape_regex(R"([\\\.\+\*\?\|\(\)\[\]\{\}\^\$])");

    auto current_pos = path_pattern.begin();
    int param_count = 0;
    std::sregex_iterator end;

    for (std::sregex_iterator temp_it(path_pattern.begin(), path_pattern.end(), param_regex); temp_it != end; ++temp_it)
    {
      param_count++;
    }

    int current_param_index = 0;

    auto it = std::sregex_iterator(path_pattern.begin(), path_pattern.end(), param_regex);

    while (it != end)
    {
      auto literal_part = std::string(current_pos, it->prefix().second);
      if (!literal_part.empty())
      {
        size_t start = 0;
        size_t found = literal_part.find('/');
        while (found != std::string::npos)
        {
          if (found > start)
          {
            literal_segments++;
          }
          start = found + 1;
          found = literal_part.find('/', start);
        }
        if (literal_part.length() > start)
        {
          literal_segments++;
        }
      }
      regex_str += std::regex_replace(literal_part, escape_regex, "\\$&");

      param_names.push_back(it->str().substr(1));
      dynamic_segments++;

      if (current_param_index == param_count - 1)
      {
        regex_str += "(.*)";
      }
      else
      {
        regex_str += "([^/]+)";
      }
      current_param_index++;

      current_pos = it->suffix().first;
      ++it;
    }
    auto tail_literal_part = std::string(current_pos, path_pattern.end());
    if (!tail_literal_part.empty())
    {
      size_t start = 0;
      size_t found = tail_literal_part.find('/');
      while (found != std::string::npos)
      {
        if (found > start)
        {
          literal_segments++;
        }
        start = found + 1;
        found = tail_literal_part.find('/', start);
      }
      if (tail_literal_part.length() > start)
      {
        literal_segments++;
      }
    }
    regex_str += std::regex_replace(tail_literal_part, escape_regex, "\\$&");
    regex_str += "$";

    return {std::regex(regex_str), param_names, literal_segments, dynamic_segments};
  }

  void HttpRouter::add_route(const std::string& path_pattern, const boost::beast::http::verb method,
                             HttpHandler handler,
                             std::optional<boost::json::value> request_schema,
                             std::optional<boost::json::value> response_schema,
                             const bool documented,
                             RouteDocumentation documentation)
  {
    if (documented)
      record_route_descriptor(path_pattern, method, std::move(request_schema), std::move(response_schema),
                              std::move(documentation));
    else
      route_descriptors_.erase(std::remove_if(route_descriptors_.begin(), route_descriptors_.end(),
        [&](const RouteDescriptor& descriptor)
        {
          return descriptor.path == path_pattern && descriptor.method == method;
        }), route_descriptors_.end());

    for (auto& entry : routes_)
    {
      if (entry.original_path == path_pattern)
      {
        entry.handlers[method] = std::move(handler);
        spdlog::debug("Updated handler for route: {} {}", std::string(boost::beast::http::to_string(method)),
                      path_pattern);
        return;
      }
    }

    RouteEntry new_entry;
    new_entry.original_path = path_pattern;
    auto [regex, params, literal_count, dynamic_count] = parse_path_pattern(path_pattern);
    new_entry.path_regex = std::move(regex);
    new_entry.param_names = std::move(params);
    new_entry.literal_segments_count = literal_count;
    new_entry.dynamic_segments_count = dynamic_count;
    new_entry.handlers[method] = std::move(handler);

    routes_.push_back(std::move(new_entry));
    std::sort(routes_.begin(), routes_.end(), RouteEntry::compare_specificity);
    spdlog::debug("Registered dynamic route: {} {} (literal:{}, dynamic:{})",
                  std::string(boost::beast::http::to_string(method)), path_pattern, literal_count, dynamic_count);
  }

  void HttpRouter::add_typed_route(const std::string& path_pattern,
                                   const boost::beast::http::verb method,
                                   detail::TypedRouteHandler handler,
                                   RouteDocumentation documentation)
  {
    add_route(path_pattern, method, std::move(handler.handler),
              std::move(handler.request_schema), std::move(handler.response_schema), true,
              std::move(documentation));
  }

  void HttpRouter::record_route_descriptor(const std::string& path,
                                           const boost::beast::http::verb method,
                                           std::optional<boost::json::value> request_schema,
                                           std::optional<boost::json::value> response_schema,
                                           RouteDocumentation documentation)
  {
    for (auto& descriptor : route_descriptors_)
    {
      if (descriptor.path == path && descriptor.method == method)
      {
        descriptor.request_schema = std::move(request_schema);
        descriptor.response_schema = std::move(response_schema);
        if (!documentation.summary.empty() || !documentation.description.empty() ||
            !documentation.headers.empty())
          descriptor.documentation = std::move(documentation);
        return;
      }
    }
    route_descriptors_.push_back(
      {path, method, std::move(request_schema), std::move(response_schema), std::move(documentation)});
  }

  std::vector<RouteDescriptor> HttpRouter::route_descriptors() const
  {
    return route_descriptors_;
  }

  void HttpRouter::document_route(const std::string& path, const boost::beast::http::verb method,
                                  RouteDocumentation documentation)
  {
    for (auto& descriptor : route_descriptors_)
    {
      if (descriptor.path == path && descriptor.method == method)
      {
        descriptor.documentation = std::move(documentation);
        return;
      }
    }
    throw std::invalid_argument("Cannot document an unregistered route: " + path);
  }

  void HttpRouter::get(const std::string& path, HttpHandler handler)
  {
    add_route(path, boost::beast::http::verb::get, std::move(handler));
  }

  void HttpRouter::get(const std::string& path, HttpHandler handler, RouteDocumentation documentation)
  {
    add_route(path, boost::beast::http::verb::get, std::move(handler), std::nullopt, std::nullopt, true,
              std::move(documentation));
  }

  void HttpRouter::post(const std::string& path, HttpHandler handler)
  {
    add_route(path, boost::beast::http::verb::post, std::move(handler));
  }

  void HttpRouter::post(const std::string& path, HttpHandler handler, RouteDocumentation documentation)
  {
    add_route(path, boost::beast::http::verb::post, std::move(handler), std::nullopt, std::nullopt, true,
              std::move(documentation));
  }

  void HttpRouter::put(const std::string& path, HttpHandler handler)
  {
    add_route(path, boost::beast::http::verb::put, std::move(handler));
  }

  void HttpRouter::put(const std::string& path, HttpHandler handler, RouteDocumentation documentation)
  {
    add_route(path, boost::beast::http::verb::put, std::move(handler), std::nullopt, std::nullopt, true,
              std::move(documentation));
  }

  void HttpRouter::del(const std::string& path, HttpHandler handler)
  {
    add_route(path, boost::beast::http::verb::delete_, std::move(handler));
  }

  void HttpRouter::del(const std::string& path, HttpHandler handler, RouteDocumentation documentation)
  {
    add_route(path, boost::beast::http::verb::delete_, std::move(handler), std::nullopt, std::nullopt, true,
              std::move(documentation));
  }

  void HttpRouter::options(const std::string& path, HttpHandler handler)
  {
    add_route(path, boost::beast::http::verb::options, std::move(handler));
  }

  void HttpRouter::options(const std::string& path, HttpHandler handler, RouteDocumentation documentation)
  {
    add_route(path, boost::beast::http::verb::options, std::move(handler), std::nullopt, std::nullopt, true,
              std::move(documentation));
  }

  void HttpRouter::stream(const std::string& path_pattern, const boost::beast::http::verb method,
                          HttpStreamHandler handler)
  {
    record_route_descriptor(path_pattern, method);
    for (auto& entry : routes_)
    {
      if (entry.original_path == path_pattern)
      {
        entry.stream_handlers[method] = std::move(handler);
        return;
      }
    }
    RouteEntry entry;
    entry.original_path = path_pattern;
    auto [regex, params, literal_count, dynamic_count] = parse_path_pattern(path_pattern);
    entry.path_regex = std::move(regex);
    entry.param_names = std::move(params);
    entry.literal_segments_count = literal_count;
    entry.dynamic_segments_count = dynamic_count;
    entry.stream_handlers[method] = std::move(handler);
    routes_.push_back(std::move(entry));
    std::sort(routes_.begin(), routes_.end(), RouteEntry::compare_specificity);
  }

  bool HttpRouter::is_stream_route(const std::string& path, const boost::beast::http::verb method) const
  {
    for (const auto& entry : routes_)
    {
      if (!std::regex_match(path, entry.path_regex)) continue;
      return entry.stream_handlers.find(method) != entry.stream_handlers.end();
    }
    return false;
  }

  bool HttpRouter::dispatch_stream(HttpContext& ctx, std::shared_ptr<HttpRequestStream> stream,
                                   std::shared_ptr<HttpResponseStream> response_stream,
                                   HttpStreamComplete complete) const
  {
    for (const auto& entry : routes_)
    {
      std::smatch matches;
      if (!std::regex_match(ctx.path(), matches, entry.path_regex)) continue;
      const auto handler = entry.stream_handlers.find(ctx.method());
      if (handler == entry.stream_handlers.end()) return false;
      std::map<std::string, std::string> params;
      for (size_t i = 0; i < entry.param_names.size() && i + 1 < matches.size(); ++i)
        params[entry.param_names[i]] = matches[i + 1].str();
      ctx.set_path_params(std::move(params));
      handler->second(ctx, std::move(stream), std::move(response_stream), std::move(complete));
      return true;
    }
    return false;
  }

  void HttpRouter::add_interceptor(std::shared_ptr<Interceptor> interceptor)
  {
    interceptors_.push_back(std::move(interceptor));
  }

  void HttpRouter::async_run_pre_interceptors(HttpContext& ctx, InterceptorCompletion complete) const
  {
    struct State : std::enable_shared_from_this<State>
    {
      const HttpRouter* router;
      const std::vector<std::shared_ptr<Interceptor>>* interceptors;
      HttpContext* ctx;
      InterceptorCompletion complete;
      std::size_t index = 0;

      void advance(InterceptorResult result)
      {
        if (result == InterceptorResult::Stop || index == interceptors->size())
          return complete(result);
        auto interceptor = (*interceptors)[index++];
        try
        {
          interceptor->async_handle_request(*ctx, [self = shared_from_this()](InterceptorResult next_result)
          { self->advance(next_result); });
        }
        catch (...)
        {
          router->handle_exception(std::current_exception(), *ctx);
          complete(InterceptorResult::Stop);
        }
      }
    };
    auto state = std::make_shared<State>();
    state->router = this;
    state->interceptors = &interceptors_;
    state->ctx = &ctx;
    state->complete = std::move(complete);
    state->advance(InterceptorResult::Continue);
  }

  void HttpRouter::async_route(const std::string& path, boost::beast::http::verb method,
                               HttpAsyncHandler handler)
  {
    record_route_descriptor(path, method);
    auto [path_regex, param_names, literal_count, dynamic_count] = parse_path_pattern(path);
    for (auto& entry : routes_)
    {
      if (entry.original_path == path)
      {
        entry.async_handlers[method] = std::move(handler);
        return;
      }
    }
    RouteEntry entry;
    entry.original_path = path;
    entry.path_regex = std::move(path_regex);
    entry.param_names = std::move(param_names);
    entry.literal_segments_count = literal_count;
    entry.dynamic_segments_count = dynamic_count;
    entry.async_handlers[method] = std::move(handler);
    routes_.push_back(std::move(entry));
    std::sort(routes_.begin(), routes_.end(), RouteEntry::compare_specificity);
  }

  bool HttpRouter::dispatch_async(HttpContext& ctx, HttpAsyncComplete complete) const
  {
    for (const auto& entry : routes_)
    {
      std::smatch matches;
      if (!std::regex_match(ctx.path(), matches, entry.path_regex)) continue;
      auto handler = entry.async_handlers.find(ctx.method());
      if (handler == entry.async_handlers.end() && ctx.method() == boost::beast::http::verb::head)
        handler = entry.async_handlers.find(boost::beast::http::verb::get);
      if (handler == entry.async_handlers.end()) return false;
      std::map<std::string, std::string> params;
      for (size_t i = 0; i < entry.param_names.size() && i + 1 < matches.size(); ++i)
        params[entry.param_names[i]] = matches[i + 1].str();
      ctx.set_path_params(std::move(params));
      handler->second(ctx, std::move(complete));
      return true;
    }
    return false;
  }

  InterceptorResult HttpRouter::run_pre_interceptors(HttpContext& ctx) const
  {
    for (const auto& interceptor : interceptors_)
    {
      if (interceptor->handle_request(ctx) == InterceptorResult::Stop)
      {
        return InterceptorResult::Stop;
      }
    }
    return InterceptorResult::Continue;
  }

  void HttpRouter::run_post_interceptors(HttpContext& ctx) const
  {
    for (auto it = interceptors_.rbegin(); it != interceptors_.rend(); ++it)
    {
      (*it)->handle_response(ctx);
    }
  }

  bool HttpRouter::dispatch(HttpContext& ctx, const std::function<bool()>& static_file_fun) const
  {
    const std::string request_path = ctx.path();
    const boost::beast::http::verb request_method = ctx.method();

    for (const auto& entry : routes_)
    {
      if (std::smatch matches; std::regex_match(request_path, matches, entry.path_regex))
      {
        auto method_it = entry.handlers.find(request_method);
        if (method_it == entry.handlers.end() && request_method == boost::beast::http::verb::head)
        {
          method_it = entry.handlers.find(boost::beast::http::verb::get);
        }
        if (method_it != entry.handlers.end())
        {
          std::map<std::string, std::string> path_params;
          for (size_t i = 0; i < entry.param_names.size(); ++i)
          {
            if (i + 1 < matches.size())
            {
              path_params[entry.param_names[i]] = matches[i + 1].str();
            }
          }
          ctx.set_path_params(std::move(path_params));

          method_it->second(ctx);
          return true;
        }
        if (request_method != boost::beast::http::verb::get && request_method != boost::beast::http::verb::head)
        {
          handle_method_not_allowed(ctx, entry.handlers); // 传递允许的方法映射
          return true;
        }
      }
    }

    if (!static_file_fun || !static_file_fun())
    {
      handle_not_found(ctx);
      return false;
    }
    return true;
  }

  void HttpRouter::handle_not_found(HttpContext& ctx)
  {
    ctx.set_status(boost::beast::http::status::not_found);
    ctx.set_content_type("text/html");
    ctx.set_body(fmt::format("<h1>404 Not Found</h1><p>The resource '{}' was not found on this server.</p>",
                             ctx.path()));
    spdlog::warn("404 Not Found: {}", ctx.path());
  }

  void HttpRouter::handle_method_not_allowed(HttpContext& ctx,
                                             const std::map<boost::beast::http::verb, HttpHandler>& allowed_methods)
  {
    ctx.set_status(boost::beast::http::status::method_not_allowed);
    ctx.set_content_type("text/html");
    ctx.set_body(fmt::format("<h1>405 Method Not Allowed</h1><p>Method {} not allowed for resource '{}'.</p>",
                             boost::beast::http::to_string(ctx.method()), ctx.path()));

    std::string allowed_methods_str;
    bool first = true;
    for (const auto& pair : allowed_methods)
    {
      if (!first) allowed_methods_str += ", ";
      allowed_methods_str += boost::beast::http::to_string(pair.first);
      first = false;
    }
    ctx.set_header(boost::beast::http::field::allow, allowed_methods_str);
    spdlog::warn("405 Method Not Allowed: {} {}", std::string(boost::beast::http::to_string(ctx.method())),
                 ctx.path());
  }

  void HttpRouter::add_exception_handler(std::shared_ptr<ExceptionHandlerBase> handler)
  {
    exception_handlers_.push_back(std::move(handler));
  }

  void HttpRouter::set_unknown_exception_handler(UnknownExceptionHandler handler)
  {
    unknown_exception_handler_ = std::move(handler);
  }

  void HttpRouter::handle_exception(std::exception_ptr eptr, HttpContext& ctx) const
  {
    reset_exception_response(ctx);

    if (!eptr)
    {
      // Should not happen, but safeguard against null pointer
      spdlog::error("handle_exception called with null exception_ptr");
      handle_unknown_exception(ctx);
      return;
    }

    try
    {
      std::rethrow_exception(eptr);
    }
    catch (const HttpException& exception)
    {
      spdlog::warn("HTTP exception: {}", exception.what());
      exception.apply(ctx);
      return;
    }
    catch (...)
    {
    }

    for (const auto& handler : exception_handlers_)
    {
      try
      {
        if (handler->try_handle(eptr, ctx))
        {
          return;
        }
      }
      catch (const std::exception& mapper_error)
      {
        spdlog::error("Exception handler failed: {}", mapper_error.what());
        write_internal_server_error(ctx);
        return;
      }
      catch (...)
      {
        spdlog::error("Exception handler failed with a non-standard exception.");
        write_internal_server_error(ctx);
        return;
      }
    }

    // Default handling for std::exception if no specific handler matched
    try
    {
      std::rethrow_exception(eptr);
    }
    catch (const std::exception& e)
    {
      spdlog::error("Unhandled exception: {}", e.what());
      write_internal_server_error(ctx);
      return;
    }
    catch (...)
    {
      // Fall through to unknown exception handler
    }

    handle_unknown_exception(ctx);
  }

  void HttpRouter::handle_unknown_exception(HttpContext& ctx) const
  {
    if (unknown_exception_handler_)
    {
      unknown_exception_handler_(ctx);
      return;
    }

    spdlog::error("Unknown exception occurred.");
    write_internal_server_error(ctx);
  }
}
