//
// Created by Caesar on 2025/6/2.
//

#ifndef HELLOWSCONTROLLER_HPP
#define HELLOWSCONTROLLER_HPP

#include "controller/http_controller.hpp"
#include <spdlog/spdlog.h>

class HelloWsController : public khttpd::framework::BaseController<HelloWsController>
{
public:
  static std::shared_ptr<HelloWsController> create() { return std::make_shared<HelloWsController>(); }

  std::shared_ptr<BaseController> register_routes(khttpd::framework::HttpRouter& router) override
  {
    KHTTPD_ROUTE(get, "/hellows", handle_hello);
    return shared_from_this();
  }

  std::shared_ptr<BaseController> register_routes(khttpd::framework::WebsocketRouter& router) override
  {
    // KHTTPD_WSROUTE("/hellows", onopen, onmessage, onclose, onerror);
    router.add_handler("/hellows",
                       bind_handler(&std::decay_t<decltype(*this)>::onopen),
                       bind_handler(&std::decay_t<decltype(*this)>::onmessage),
                       bind_handler(&std::decay_t<decltype(*this)>::onclose),
                       bind_handler(&std::decay_t<decltype(*this)>::onerror));
    return shared_from_this();
  }

private:
  void handle_hello(khttpd::framework::HttpContext& ctx)
  {
    ctx.set_status(boost::beast::http::status::upgrade_required);
    ctx.set_header(boost::beast::http::field::upgrade, "websocket");
    ctx.set_content_type("text/html");
    ctx.set_body(
      "<h1>WebSocket Echo Endpoint</h1><p>This is a WebSocket endpoint. Please use a WebSocket client to connect.</p>");
  }

  void onopen(khttpd::framework::WebsocketContext& ctx)
  {
    spdlog::info("[WS: {}] Connection opened.", ctx.path);
    ctx.send("Welcome to the echo service!");
  }

  void onclose(khttpd::framework::WebsocketContext& ctx)
  {
    spdlog::info("[WS: {}] Connection closed.", ctx.path);
  }

  void onerror(khttpd::framework::WebsocketContext& ctx)
  {
    spdlog::error("[WS: {}] Error: {}", ctx.path, ctx.error_code.message());
  }

  void onmessage(khttpd::framework::WebsocketContext& ctx)
  {
    spdlog::info("[WS: {}] Received: {}", ctx.path, ctx.message);
    ctx.send("Echo: " + ctx.message, ctx.is_text);
  }
};

#endif //HELLOWSCONTROLLER_HPP
