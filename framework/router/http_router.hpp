// framework/router/http_router.hpp
#ifndef KHTTPD_FRAMEWORK_ROUTER_HTTP_ROUT
#define KHTTPD_FRAMEWORK_ROUTER_HTTP_ROUT

#include "context/http_context.hpp"
#include "context/http_request_stream.hpp"
#include "context/http_response_stream.hpp"
#include "interceptor/interceptor.hpp"
#include "exception/exception_handler.hpp"
#include "exception/http_exception.hpp"
#include "router/typed_route.hpp"
#include <functional>
#include <string>
#include <map>
#include <vector>
#include <regex>
#include <memory>
#include <optional>

namespace khttpd::framework
{
  struct OpenApiInfo;
  class HttpRouter;
  void install_openapi_routes(HttpRouter& router, const OpenApiInfo& info,
                              const std::string& spec_path, const std::string& docs_path, bool enabled);

  using HttpHandler = std::function<void(HttpContext&)>;
  using HttpAsyncComplete = std::function<void()>;
  using HttpAsyncHandler = std::function<void(HttpContext&, HttpAsyncComplete)>;
  using HttpStreamComplete = std::function<void()>;
  using HttpStreamHandler = std::function<void(HttpContext&, std::shared_ptr<HttpRequestStream>,
                                               std::shared_ptr<HttpResponseStream>, HttpStreamComplete)>;
  using UnknownExceptionHandler = std::function<void(HttpContext&)>;

  struct RouteHeader
  {
    std::string name;
    std::string description;
    bool required = false;
  };

  struct RouteDocumentation
  {
    std::string summary;
    std::string description;
    std::vector<RouteHeader> headers;
    std::optional<boost::json::value> request_schema;
    std::optional<boost::json::value> response_schema;
  };

  struct RouteDescriptor
  {
    std::string path;
    boost::beast::http::verb method;
    std::optional<boost::json::value> request_schema;
    std::optional<boost::json::value> response_schema;
    RouteDocumentation documentation;
  };

  // 路由条目结构
  struct RouteEntry
  {
    std::string original_path;
    std::regex path_regex;
    std::vector<std::string> param_names;
    std::map<boost::beast::http::verb, HttpHandler> handlers;
    std::map<boost::beast::http::verb, HttpAsyncHandler> async_handlers;
    std::map<boost::beast::http::verb, HttpStreamHandler> stream_handlers;
    int literal_segments_count = 0;
    int dynamic_segments_count = 0;

    // 比较函数：用于根据特异性对路由进行排序
    // 返回 true 表示 a 应该排在 b 之前 (a 更具体)
    static bool compare_specificity(const RouteEntry& a, const RouteEntry& b)
    {
      // 首先比较字面路径段数量，多的优先
      if (a.literal_segments_count != b.literal_segments_count)
      {
        return a.literal_segments_count > b.literal_segments_count;
      }
      // 如果字面路径段数量相同，则比较动态路径段数量，少的优先
      return a.dynamic_segments_count < b.dynamic_segments_count;
    }
  };

  class HttpRouter
  {
  public:
    HttpRouter();

    void get(const std::string& path, HttpHandler handler);
    void post(const std::string& path, HttpHandler handler);
    void put(const std::string& path, HttpHandler handler);
    void del(const std::string& path, HttpHandler handler);
    void options(const std::string& path, HttpHandler handler);

    void get(const std::string& path, HttpHandler handler, RouteDocumentation documentation);
    void post(const std::string& path, HttpHandler handler, RouteDocumentation documentation);
    void put(const std::string& path, HttpHandler handler, RouteDocumentation documentation);
    void del(const std::string& path, HttpHandler handler, RouteDocumentation documentation);
    void options(const std::string& path, HttpHandler handler, RouteDocumentation documentation);

    template <class Handler, std::enable_if_t<!std::is_convertible_v<Handler, HttpHandler>, int> = 0>
    void get(const std::string& path, Handler&& handler)
    {
      add_typed_route(path, boost::beast::http::verb::get,
                      detail::make_typed_handler(std::forward<Handler>(handler)));
    }

    template <class Handler, std::enable_if_t<!std::is_convertible_v<Handler, HttpHandler>, int> = 0>
    void post(const std::string& path, Handler&& handler)
    {
      add_typed_route(path, boost::beast::http::verb::post,
                      detail::make_typed_handler(std::forward<Handler>(handler)));
    }

    template <class Handler, std::enable_if_t<!std::is_convertible_v<Handler, HttpHandler>, int> = 0>
    void put(const std::string& path, Handler&& handler)
    {
      add_typed_route(path, boost::beast::http::verb::put,
                      detail::make_typed_handler(std::forward<Handler>(handler)));
    }

    template <class Handler, std::enable_if_t<!std::is_convertible_v<Handler, HttpHandler>, int> = 0>
    void del(const std::string& path, Handler&& handler)
    {
      add_typed_route(path, boost::beast::http::verb::delete_,
                      detail::make_typed_handler(std::forward<Handler>(handler)));
    }

    template <class Handler, std::enable_if_t<!std::is_convertible_v<Handler, HttpHandler>, int> = 0>
    void options(const std::string& path, Handler&& handler)
    {
      add_typed_route(path, boost::beast::http::verb::options,
                      detail::make_typed_handler(std::forward<Handler>(handler)));
    }

    template <class Handler, std::enable_if_t<!std::is_convertible_v<Handler, HttpHandler>, int> = 0>
    void get(const std::string& path, Handler&& handler, RouteDocumentation documentation)
    {
      add_typed_route(path, boost::beast::http::verb::get,
                      detail::make_typed_handler(std::forward<Handler>(handler)), std::move(documentation));
    }

    template <class Handler, std::enable_if_t<!std::is_convertible_v<Handler, HttpHandler>, int> = 0>
    void post(const std::string& path, Handler&& handler, RouteDocumentation documentation)
    {
      add_typed_route(path, boost::beast::http::verb::post,
                      detail::make_typed_handler(std::forward<Handler>(handler)), std::move(documentation));
    }

    template <class Handler, std::enable_if_t<!std::is_convertible_v<Handler, HttpHandler>, int> = 0>
    void put(const std::string& path, Handler&& handler, RouteDocumentation documentation)
    {
      add_typed_route(path, boost::beast::http::verb::put,
                      detail::make_typed_handler(std::forward<Handler>(handler)), std::move(documentation));
    }

    template <class Handler, std::enable_if_t<!std::is_convertible_v<Handler, HttpHandler>, int> = 0>
    void del(const std::string& path, Handler&& handler, RouteDocumentation documentation)
    {
      add_typed_route(path, boost::beast::http::verb::delete_,
                      detail::make_typed_handler(std::forward<Handler>(handler)), std::move(documentation));
    }

    template <class Handler, std::enable_if_t<!std::is_convertible_v<Handler, HttpHandler>, int> = 0>
    void options(const std::string& path, Handler&& handler, RouteDocumentation documentation)
    {
      add_typed_route(path, boost::beast::http::verb::options,
                      detail::make_typed_handler(std::forward<Handler>(handler)), std::move(documentation));
    }

    template <class Controller, class Method>
    void get(const std::string& path, std::shared_ptr<Controller> controller, Method method)
    {
      add_typed_route(path, boost::beast::http::verb::get,
                      detail::make_typed_member_handler(std::move(controller), method));
    }

    template <class Controller, class Method>
    void post(const std::string& path, std::shared_ptr<Controller> controller, Method method)
    {
      add_typed_route(path, boost::beast::http::verb::post,
                      detail::make_typed_member_handler(std::move(controller), method));
    }

    template <class Controller, class Method>
    void put(const std::string& path, std::shared_ptr<Controller> controller, Method method)
    {
      add_typed_route(path, boost::beast::http::verb::put,
                      detail::make_typed_member_handler(std::move(controller), method));
    }

    template <class Controller, class Method>
    void del(const std::string& path, std::shared_ptr<Controller> controller, Method method)
    {
      add_typed_route(path, boost::beast::http::verb::delete_,
                      detail::make_typed_member_handler(std::move(controller), method));
    }

    template <class Controller, class Method>
    void options(const std::string& path, std::shared_ptr<Controller> controller, Method method)
    {
      add_typed_route(path, boost::beast::http::verb::options,
                      detail::make_typed_member_handler(std::move(controller), method));
    }

    template <class Controller, class Method>
    void get(const std::string& path, std::shared_ptr<Controller> controller, Method method,
             RouteDocumentation documentation)
    {
      add_typed_route(path, boost::beast::http::verb::get,
                      detail::make_typed_member_handler(std::move(controller), method), std::move(documentation));
    }

    template <class Controller, class Method>
    void post(const std::string& path, std::shared_ptr<Controller> controller, Method method,
              RouteDocumentation documentation)
    {
      add_typed_route(path, boost::beast::http::verb::post,
                      detail::make_typed_member_handler(std::move(controller), method), std::move(documentation));
    }

    template <class Controller, class Method>
    void put(const std::string& path, std::shared_ptr<Controller> controller, Method method,
             RouteDocumentation documentation)
    {
      add_typed_route(path, boost::beast::http::verb::put,
                      detail::make_typed_member_handler(std::move(controller), method), std::move(documentation));
    }

    template <class Controller, class Method>
    void del(const std::string& path, std::shared_ptr<Controller> controller, Method method,
             RouteDocumentation documentation)
    {
      add_typed_route(path, boost::beast::http::verb::delete_,
                      detail::make_typed_member_handler(std::move(controller), method), std::move(documentation));
    }

    template <class Controller, class Method>
    void options(const std::string& path, std::shared_ptr<Controller> controller, Method method,
                 RouteDocumentation documentation)
    {
      add_typed_route(path, boost::beast::http::verb::options,
                      detail::make_typed_member_handler(std::move(controller), method), std::move(documentation));
    }
    // Async handlers must invoke complete exactly once, from any thread.
    void async_route(const std::string& path, boost::beast::http::verb method, HttpAsyncHandler handler);
    void stream(const std::string& path, boost::beast::http::verb method, HttpStreamHandler handler);

    // Used by HttpSession after it has parsed only the request header.
    bool is_stream_route(const std::string& path, boost::beast::http::verb method) const;
    bool dispatch_stream(HttpContext& ctx, std::shared_ptr<HttpRequestStream> stream,
                         std::shared_ptr<HttpResponseStream> response_stream,
                         HttpStreamComplete complete) const;

    void add_interceptor(std::shared_ptr<Interceptor> interceptor);
    InterceptorResult run_pre_interceptors(HttpContext& ctx) const;
    using InterceptorCompletion = std::function<void(InterceptorResult)>;
    void async_run_pre_interceptors(HttpContext& ctx, InterceptorCompletion complete) const;
    void run_post_interceptors(HttpContext& ctx) const;

    // Exception handling
    void add_exception_handler(std::shared_ptr<ExceptionHandlerBase> handler);

    template <class Exception, class Mapper>
    void map_exception(Mapper&& mapper)
    {
      using StoredMapper = std::decay_t<Mapper>;
      add_exception_handler(std::make_shared<detail::TypedExceptionMapper<Exception, StoredMapper>>(
        StoredMapper(std::forward<Mapper>(mapper))));
    }

    void set_unknown_exception_handler(UnknownExceptionHandler handler);
    void handle_exception(std::exception_ptr eptr, HttpContext& ctx) const;
    void handle_unknown_exception(HttpContext& ctx) const;

    bool dispatch(HttpContext& ctx, const std::function<bool()>& static_file_fun = nullptr) const;
    bool dispatch_async(HttpContext& ctx, HttpAsyncComplete complete) const;

    // Returns handler-free copies suitable for documentation and inspection.
    std::vector<RouteDescriptor> route_descriptors() const;

    // Adds or replaces OpenAPI summary and description for a registered route.
    void document_route(const std::string& path, boost::beast::http::verb method,
                        RouteDocumentation documentation);

  private:
    friend void install_openapi_routes(HttpRouter& router, const OpenApiInfo& info,
                                       const std::string& spec_path, const std::string& docs_path, bool enabled);

    std::vector<RouteEntry> routes_;
    std::vector<RouteDescriptor> route_descriptors_;
    std::vector<std::shared_ptr<Interceptor>> interceptors_;

    std::vector<std::shared_ptr<ExceptionHandlerBase>> exception_handlers_;
    UnknownExceptionHandler unknown_exception_handler_;

    void add_route(const std::string& path_pattern, boost::beast::http::verb method, HttpHandler handler,
                   std::optional<boost::json::value> request_schema = std::nullopt,
                   std::optional<boost::json::value> response_schema = std::nullopt,
                   bool documented = true,
                   RouteDocumentation documentation = {});
    void add_typed_route(const std::string& path_pattern, boost::beast::http::verb method,
                         detail::TypedRouteHandler handler, RouteDocumentation documentation = {});
    void record_route_descriptor(const std::string& path, boost::beast::http::verb method,
                                 std::optional<boost::json::value> request_schema = std::nullopt,
                                 std::optional<boost::json::value> response_schema = std::nullopt,
                                 RouteDocumentation documentation = {});

    static std::tuple<std::regex, std::vector<std::string>, int, int> parse_path_pattern(
      const std::string& path_pattern);

    static void handle_not_found(HttpContext& ctx);
    static void handle_method_not_allowed(HttpContext& ctx,
                                          const std::map<boost::beast::http::verb, HttpHandler>& allowed_methods);
  };
}
#endif // KHTTPD_FRAMEWORK_ROUTER_HTTP_ROUT
