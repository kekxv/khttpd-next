// framework/websocket/websocket_session.hpp
#ifndef KHTTPD_FRAMEWORK_WEBSOCKET_SESSION_HPP
#define KHTTPD_FRAMEWORK_WEBSOCKET_SESSION_HPP

#include <boost/beast.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <memory>
#include <string>
#include <queue>
#include <boost/uuid/uuid_generators.hpp>
#include "router/websocket_router.hpp"

namespace khttpd::framework
{
  namespace beast = boost::beast;
  namespace http = beast::http;
  namespace ws = beast::websocket;
  namespace net = boost::asio;
  using tcp = boost::asio::ip::tcp;

  class WebsocketSession : public std::enable_shared_from_this<WebsocketSession>
  {
  public:
    WebsocketSession(tcp::socket&& socket, WebsocketRouter& ws_router, const std::string& initial_path);
    virtual ~WebsocketSession() = default;

    template <class Body, class Allocator>
    void run_handshake(http::request<Body, http::basic_fields<Allocator>> req);

    virtual void send_message(const std::string& msg, bool is_text);

    static bool send_message(const std::string& id, const std::string& msg, bool is_text);
    static size_t send_message(const std::vector<std::string>& ids, const std::string& msg, bool is_text);

  public:
    std::string id;

  private:
    ws::stream<beast::tcp_stream> ws_;
    beast::flat_buffer buffer_;
    WebsocketRouter& websocket_router_;
    std::string initial_path_;
    static std::mutex m_gen_mutex;
    static boost::uuids::random_generator gen;
    static std::mutex m_sessions_mutex;
    static std::map<std::string, std::shared_ptr<WebsocketSession>> m_sessions_id_;

    // --- 新增常量 ---
    // 定义分片大小，例如 16KB。这是一个可以调整的参数。
    static constexpr size_t const fragment_size_ = 16 * 1024;
    // 定义一个阈值，小于这个大小的消息不进行分片，直接发送。
    static constexpr size_t const auto_fragment_threshold_ = fragment_size_ * 2;

    // Write queue to serialize concurrent async_write calls
    std::queue<std::pair<std::shared_ptr<const std::string>, bool>> write_queue_;
    bool writing_ = false;

    void on_handshake(beast::error_code ec);
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void do_write_next();
    void on_write(beast::error_code ec, std::size_t bytes_transferred);
    void do_close(beast::error_code ec = {});
  };

  template <class Body, class Allocator>
  void WebsocketSession::run_handshake(http::request<Body, http::basic_fields<Allocator>> req)
  {
    ws_.async_accept(req,
                     beast::bind_front_handler(&WebsocketSession::on_handshake, shared_from_this()));
  }
}
#endif // KHTTPD_FRAMEWORK_WEBSOCKET_SESSION_HPP
