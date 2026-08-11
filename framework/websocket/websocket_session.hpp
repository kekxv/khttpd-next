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
    virtual void send_frame(WebsocketFrame frame);

    const WebsocketHandshakeRequest& handshake() const { return handshake_; }

    static bool send_message(const std::string& id, const std::string& msg, bool is_text);
    static size_t send_message(const std::vector<std::string>& ids, const std::string& msg, bool is_text);

  public:
    std::string id;

  private:
    ws::stream<beast::tcp_stream> ws_;
    beast::flat_buffer buffer_;
    WebsocketRouter& websocket_router_;
    std::string initial_path_;
    WebsocketHandshakeRequest handshake_;
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
    std::queue<WebsocketFrame> write_queue_;
    bool writing_ = false;
    bool closed_ = false;
    bool close_pending_ = false;

    void on_handshake(beast::error_code ec);
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void do_write_next();
    void on_write(beast::error_code ec, std::size_t bytes_transferred);
    void close_stream();
    void do_close(beast::error_code ec = {});
  };

  template <class Body, class Allocator>
  void WebsocketSession::run_handshake(http::request<Body, http::basic_fields<Allocator>> req)
  {
    handshake_.target = std::string(req.target());
    const auto query_pos = handshake_.target.find('?');
    handshake_.path = handshake_.target.substr(0, query_pos);
    initial_path_ = handshake_.path.empty() ? "/" : handshake_.path;
    for (const auto& field : req)
    {
      const std::string name(field.name_string());
      const std::string value(field.value());
      handshake_.headers.emplace_back(name, value);
      if (field.name() == http::field::sec_websocket_protocol)
      {
        size_t begin = 0;
        while (begin < value.size())
        {
          const size_t end = value.find(',', begin);
          const auto token = value.substr(begin, end == std::string::npos ? end : end - begin);
          const auto first = token.find_first_not_of(" \t");
          if (first != std::string::npos) handshake_.subprotocols.push_back(token.substr(first, token.find_last_not_of(" \t") - first + 1));
          if (end == std::string::npos) break;
          begin = end + 1;
        }
      }
    }
    if (query_pos != std::string::npos)
    {
      std::string query = handshake_.target.substr(query_pos + 1);
      size_t begin = 0;
      while (begin <= query.size())
      {
        const size_t end = query.find('&', begin);
        const std::string pair = query.substr(begin, end == std::string::npos ? end : end - begin);
        const size_t equal = pair.find('=');
        handshake_.query_params.emplace(pair.substr(0, equal), equal == std::string::npos ? "" : pair.substr(equal + 1));
        if (end == std::string::npos) break;
        begin = end + 1;
      }
    }
    ws_.async_accept(req,
                     beast::bind_front_handler(&WebsocketSession::on_handshake, shared_from_this()));
  }
}
#endif // KHTTPD_FRAMEWORK_WEBSOCKET_SESSION_HPP
