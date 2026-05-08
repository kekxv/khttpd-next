#ifndef KHTTPD_FRAMEWORK_WEBSOCKET_CONTEXT_HPP
#define KHTTPD_FRAMEWORK_WEBSOCKET_CONTEXT_HPP

#include <string>
#include <memory>
#include <boost/beast/core/error.hpp>
#include <map>
#include <any>
#include <optional>

namespace khttpd::framework
{
  class WebsocketSession;
  class WebsocketContext
  {
  public:
    std::string id;
    std::weak_ptr<WebsocketSession> session_weak_ptr;
    std::string message;
    bool is_text;
    boost::beast::error_code error_code;
    std::string path;

    std::map<std::string, std::any> extended_data;

    WebsocketContext(std::weak_ptr<WebsocketSession> session, std::string  msg, bool text,
                     std::string  path);
    WebsocketContext(std::weak_ptr<WebsocketSession> session, std::string  path,
                     boost::beast::error_code ec = {});

    void send(const std::string& msg, bool is_text = true);

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
  };
}
#endif // KHTTPD_FRAMEWORK_WEBSOCKET_CONTEXT_HPP
