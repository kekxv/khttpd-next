// framework/router/websocket_router.hpp
#ifndef KHTTPD_FRAMEWORK_ROUTER_WEBSOCKET_ROUTER_HPP
#define KHTTPD_FRAMEWORK_ROUTER_WEBSOCKET_ROUTER_HPP

#include <functional>
#include <string>
#include <map>
#include <regex>
#include <shared_mutex>
#include <tuple>
#include <vector>
#include "context/websocket_context.hpp"

namespace khttpd::framework
{
  namespace websocket
  {
    bool send_message(const std::string& id, const std::string& msg, bool is_text);
    size_t send_message(const std::vector<std::string>& ids, const std::string& msg, bool is_text);
  }

  using WebsocketOpenHandler = std::function<void(WebsocketContext&)>;
  using WebsocketMessageHandler = std::function<void(WebsocketContext&)>;
  using WebsocketCloseHandler = std::function<void(WebsocketContext&)>;
  using WebsocketErrorHandler = std::function<void(WebsocketContext&)>;

  // 路由条目，包含所有事件的处理器
  struct WebsocketRouteEntry
  {
    WebsocketOpenHandler on_open;
    WebsocketMessageHandler on_message;
    WebsocketCloseHandler on_close;
    WebsocketErrorHandler on_error;
  };

  struct WebsocketRoute
  {
    std::string original_path;
    std::regex path_regex;
    std::vector<std::string> param_names;
    int literal_segments_count = 0;
    int dynamic_segments_count = 0;
    WebsocketRouteEntry handlers;
    static bool compare_specificity(const WebsocketRoute& a, const WebsocketRoute& b)
    {
      return a.literal_segments_count == b.literal_segments_count
        ? a.dynamic_segments_count < b.dynamic_segments_count
        : a.literal_segments_count > b.literal_segments_count;
    }
  };

  class WebsocketRouter
  {
  public:
    WebsocketRouter();

    void add_handler(const std::string& path,
                     WebsocketOpenHandler on_open = {nullptr},
                     WebsocketMessageHandler on_message = {nullptr},
                     WebsocketCloseHandler on_close = {nullptr},
                     WebsocketErrorHandler on_error = {nullptr});

    void dispatch_open(const std::string& path, WebsocketContext& ctx);
    void dispatch_message(const std::string& path, WebsocketContext& ctx);
    void dispatch_close(const std::string& path, WebsocketContext& ctx);
    void dispatch_error(const std::string& path, WebsocketContext& ctx);

  private:
    static std::tuple<std::regex, std::vector<std::string>, int, int> parse_path_pattern(const std::string& pattern);
    void dispatch(const std::string& path, WebsocketContext& ctx,
                  const std::function<void(const WebsocketRouteEntry&)>& invoke);
    std::vector<WebsocketRoute> handlers_;
    mutable std::shared_mutex handlers_mutex_;
  };
}
#endif // KHTTPD_FRAMEWORK_ROUTER_WEBSOCKET_ROUTER_HPP
