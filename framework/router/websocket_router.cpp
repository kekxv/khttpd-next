#include "websocket_router.hpp"

#include <algorithm>
#include <spdlog/spdlog.h>
#include "websocket/websocket_session.hpp"

namespace khttpd::framework
{
  bool websocket::send_message(const std::string& id, const std::string& msg, const bool is_text)
  {
    return WebsocketSession::send_message(id, msg, is_text);
  }

  size_t websocket::send_message(const std::vector<std::string>& ids, const std::string& msg, const bool is_text)
  {
    return WebsocketSession::send_message(ids, msg, is_text);
  }

  WebsocketRouter::WebsocketRouter() = default;

  void WebsocketRouter::add_handler(const std::string& path, WebsocketOpenHandler on_open,
                                    WebsocketMessageHandler on_message, WebsocketCloseHandler on_close,
                                    WebsocketErrorHandler on_error)
  {
    WebsocketRouteEntry entry{std::move(on_open), std::move(on_message), std::move(on_close), std::move(on_error)};
    std::unique_lock lock(handlers_mutex_);
    for (auto& route : handlers_)
      if (route.original_path == path) { route.handlers = std::move(entry); return; }
    auto [regex, params, literal_count, dynamic_count] = parse_path_pattern(path);
    handlers_.push_back({path, std::move(regex), std::move(params), literal_count, dynamic_count, std::move(entry)});
    std::sort(handlers_.begin(), handlers_.end(), WebsocketRoute::compare_specificity);
    spdlog::debug("Registered WebSocket handlers for path: {}", path);
  }

  void WebsocketRouter::dispatch_open(const std::string& path, WebsocketContext& ctx)
  { dispatch(path, ctx, [&](const WebsocketRouteEntry& h) { if (h.on_open) h.on_open(ctx); }); }
  void WebsocketRouter::dispatch_message(const std::string& path, WebsocketContext& ctx)
  { dispatch(path, ctx, [&](const WebsocketRouteEntry& h) { if (h.on_message) h.on_message(ctx); }); }
  void WebsocketRouter::dispatch_close(const std::string& path, WebsocketContext& ctx)
  { dispatch(path, ctx, [&](const WebsocketRouteEntry& h) { if (h.on_close) h.on_close(ctx); }); }
  void WebsocketRouter::dispatch_error(const std::string& path, WebsocketContext& ctx)
  { dispatch(path, ctx, [&](const WebsocketRouteEntry& h) { if (h.on_error) h.on_error(ctx); }); }

  std::tuple<std::regex, std::vector<std::string>, int, int>
  WebsocketRouter::parse_path_pattern(const std::string& pattern)
  {
    std::string expression = "^";
    std::vector<std::string> names;
    std::regex parameter(":([a-zA-Z_][a-zA-Z0-9_]*)");
    std::regex escaped(R"([\\\.\+\*\?\|\(\)\[\]\{\}\^\$])");
    std::sregex_iterator end, it(pattern.begin(), pattern.end(), parameter);
    const int total = std::distance(it, end);
    auto cursor = pattern.begin();
    int literals = 0, dynamics = 0;
    for (int index = 0; it != end; ++it, ++index)
    {
      const std::string literal(cursor, it->prefix().second);
      literals += static_cast<int>(std::count(literal.begin(), literal.end(), '/'));
      expression += std::regex_replace(literal, escaped, "\\$&");
      names.push_back((*it)[1]); ++dynamics;
      expression += index + 1 == total ? "(.*)" : "([^/]+)";
      cursor = it->suffix().first;
    }
    const std::string tail(cursor, pattern.end());
    literals += static_cast<int>(std::count(tail.begin(), tail.end(), '/'));
    expression += std::regex_replace(tail, escaped, "\\$&");
    return {std::regex(expression + "$"), names, literals, dynamics};
  }

  void WebsocketRouter::dispatch(const std::string& path, WebsocketContext& ctx,
                                 const std::function<void(const WebsocketRouteEntry&)>& invoke)
  {
    {
      std::shared_lock lock(handlers_mutex_);
      for (const auto& route : handlers_)
      {
        std::smatch match;
        if (!std::regex_match(path, match, route.path_regex)) continue;
        std::map<std::string, std::string> params;
        for (size_t i = 0; i < route.param_names.size(); ++i) params[route.param_names[i]] = match[i + 1].str();
        ctx.set_path_params(std::move(params));
        const auto handler = route.handlers;
        lock.unlock();
        invoke(handler);
        return;
      }
    }
    spdlog::warn("No WebSocket handler found for path: {}", path);
  }
}
