// framework/context/websocket_context.cpp
#include "websocket_context.hpp"
#include "websocket/websocket_session.hpp"
#include <spdlog/spdlog.h>

#include <utility>
#include <algorithm>
#include <cctype>

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
    frame.type = text ? WebsocketFrameType::text : WebsocketFrameType::binary;
    frame.payload = message;
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

  void WebsocketContext::send(WebsocketFrame outbound_frame)
  {
    if (auto session = session_weak_ptr.lock()) session->send_frame(std::move(outbound_frame));
    else spdlog::error("Attempted to send WS frame to expired session (path: {}).", path);
  }

  const WebsocketHandshakeRequest& WebsocketContext::handshake() const
  {
    static const WebsocketHandshakeRequest empty;
    if (const auto session = session_weak_ptr.lock()) return session->handshake();
    return empty;
  }

  std::optional<std::string> WebsocketContext::get_header(const std::string& name) const
  {
    const auto values = get_headers(name);
    if (values.empty()) return std::nullopt;
    return values.front();
  }

  std::vector<std::string> WebsocketContext::get_headers(const std::string& name) const
  {
    std::vector<std::string> values;
    const auto equal_ci = [](const std::string& a, const std::string& b)
    {
      return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
    };
    for (const auto& [key, value] : handshake().headers)
      if (equal_ci(key, name)) values.push_back(value);
    return values;
  }

  std::optional<std::string> WebsocketContext::get_query_param(const std::string& key) const
  {
    const auto it = handshake().query_params.find(key);
    return it == handshake().query_params.end() ? std::nullopt : std::optional<std::string>(it->second);
  }

  std::optional<std::string> WebsocketContext::get_path_param(const std::string& key) const
  {
    const auto it = path_params_.find(key);
    return it == path_params_.end() ? std::nullopt : std::optional<std::string>(it->second);
  }

  void WebsocketContext::set_path_params(std::map<std::string, std::string> params)
  {
    path_params_ = std::move(params);
  }
}
