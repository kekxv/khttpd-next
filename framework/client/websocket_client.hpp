#ifndef KHTTPD_FRAMEWORK_CLIENT_WEBSOCKET_CLIENT_HPP
#define KHTTPD_FRAMEWORK_CLIENT_WEBSOCKET_CLIENT_HPP

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <deque>

namespace khttpd::framework::client
{
  namespace beast = boost::beast;
  namespace websocket = beast::websocket;
  namespace net = boost::asio;
  namespace ssl = boost::asio::ssl;
  using tcp = boost::asio::ip::tcp;

  // 前置声明内部会话接口
  struct WebsocketSessionImpl;

  class WebsocketClient : public std::enable_shared_from_this<WebsocketClient>
  {
  public:
    struct State;

    using ConnectCallback = std::function<void(beast::error_code)>;
    using MessageHandler = std::function<void(const std::string&)>;
    using ErrorHandler = std::function<void(beast::error_code)>;
    using CloseHandler = std::function<void()>;

    WebsocketClient();
    // 构造函数：支持默认 SSL 或 外部 SSL Context
    explicit WebsocketClient(net::io_context& ioc);
    WebsocketClient(net::io_context& ioc, ssl::context& ssl_ctx);
    ~WebsocketClient();

    // 连接 URL (支持 ws:// 和 wss://)
    void connect(const std::string& url, ConnectCallback callback);

    // 发送消息 (线程安全，支持并发调用)
    void send(const std::string& message);

    // 关闭连接
    void close();

    // 配置
    void set_header(const std::string& key, const std::string& value);
    void set_on_message(MessageHandler handler);
    void set_on_error(ErrorHandler handler);
    void set_on_close(CloseHandler handler);

  private:
    friend WebsocketSessionImpl;
    net::io_context& ioc_;

    // SSL Context Management
    std::shared_ptr<ssl::context> own_ssl_ctx_;
    ssl::context* ssl_ctx_ptr_;

    std::shared_ptr<State> state_;

    // Headers to send during handshake
    std::map<std::string, std::string> headers_;

    // 多态的内部会话 (持有实际的 websocket stream)
    std::shared_ptr<WebsocketSessionImpl> session_;
  };
}

#endif // KHTTPD_FRAMEWORK_CLIENT_WEBSOCKET_CLIENT_HPP
