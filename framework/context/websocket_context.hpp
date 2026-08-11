#ifndef KHTTPD_FRAMEWORK_WEBSOCKET_CONTEXT_HPP
#define KHTTPD_FRAMEWORK_WEBSOCKET_CONTEXT_HPP

#include <string>
#include <memory>
#include <boost/beast/core/error.hpp>
#include <map>
#include <any>
#include <optional>
#include <vector>

namespace khttpd::framework
{
  class WebsocketSession;

  enum class WebsocketFrameType { text, binary, ping, pong, close };

  struct WebsocketFrame
  {
    WebsocketFrameType type = WebsocketFrameType::text;
    std::string payload;
    uint16_t close_code = 1000;
    std::string close_reason;
  };

  // HeaderList deliberately keeps duplicate fields (notably Cookie and
  // Sec-WebSocket-Extensions) in their original handshake order.
  using WebsocketHeaderList = std::vector<std::pair<std::string, std::string>>;

  struct WebsocketHandshakeRequest
  {
    std::string target;
    std::string path;
    WebsocketHeaderList headers;
    std::map<std::string, std::string> query_params;
    std::vector<std::string> subprotocols;
  };

  class WebsocketContext
  {
  public:
    std::string id;
    std::weak_ptr<WebsocketSession> session_weak_ptr;
    std::string message;
    bool is_text;
    boost::beast::error_code error_code;
    std::string path;
    WebsocketFrame frame;

    std::map<std::string, std::any> extended_data;

    WebsocketContext(std::weak_ptr<WebsocketSession> session, std::string  msg, bool text,
                     std::string  path);
    WebsocketContext(std::weak_ptr<WebsocketSession> session, std::string  path,
                     boost::beast::error_code ec = {});

    void send(const std::string& msg, bool is_text = true);
    void send(WebsocketFrame frame);

    const WebsocketHandshakeRequest& handshake() const;
    std::optional<std::string> get_header(const std::string& name) const;
    std::vector<std::string> get_headers(const std::string& name) const;
    std::optional<std::string> get_query_param(const std::string& key) const;
    std::optional<std::string> get_path_param(const std::string& key) const;
    void set_path_params(std::map<std::string, std::string> params);

    void set_attribute(const std::string& key, std::any value) {
        extended_data[key] = std::move(value);
    }

    std::any get_attribute(const std::string& key) const {
        auto it = extended_data.find(key);
        if (it != extended_data.end()) {
            return it->second;
        }
        return {};
    }

    template<typename T>
    std::optional<T> get_attribute_as(const std::string& key) const {
        auto it = extended_data.find(key);
        if (it != extended_data.end()) {
            try {
                return std::any_cast<T>(it->second);
            } catch (const std::bad_any_cast&) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

  private:
    std::map<std::string, std::string> path_params_;
  };
}
#endif // KHTTPD_FRAMEWORK_WEBSOCKET_CONTEXT_HPP
