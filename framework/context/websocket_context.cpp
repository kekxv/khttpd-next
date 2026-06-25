// framework/context/websocket_context.cpp
#include "websocket_context.hpp"
#include "websocket/websocket_session.hpp"
#include <spdlog/spdlog.h>

#include <utility>

namespace khttpd::framework
{
  WebsocketContext::WebsocketContext(std::weak_ptr<WebsocketSession> session, std::string msg, bool text,
                                     std::string path_str)
    : session_weak_ptr(std::move(session)), message(std::move(msg)), is_text(text), path(std::move(path_str))
  {
    if (const auto session_shared_ptr = session_weak_ptr.lock())
    {
      id = session_shared_ptr->id;
    }
  }

  WebsocketContext::WebsocketContext(std::weak_ptr<WebsocketSession> session, std::string path_str,
                                     boost::beast::error_code ec)
    : session_weak_ptr(std::move(session)), is_text(false), error_code(ec), path(std::move(path_str))
  {
    if (const auto session_shared_ptr = session_weak_ptr.lock())
    {
      id = session_shared_ptr->id;
    }
  }


  void WebsocketContext::send(const std::string& msg, bool is_text_msg)
  {
    if (auto session_shared_ptr = session_weak_ptr.lock())
    {
      session_shared_ptr->send_message(msg, is_text_msg);
    }
    else
    {
      spdlog::error("Attempted to send WS message to expired session (path: {}).", path);
    }
  }
}
