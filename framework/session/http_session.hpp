#ifndef KHTTPD_HTTP_SESSION_HPP
#define KHTTPD_HTTP_SESSION_HPP

#include <boost/filesystem.hpp>
#include <boost/beast.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <memory>
#include <queue>
#include <sstream>
#include <array>
#include <cstdint>
#include "router/http_router.hpp"
#include "websocket/websocket_session.hpp"


namespace khttpd::framework
{
  namespace beast = boost::beast;
  namespace http = beast::http;
  namespace net = boost::asio;
  using tcp = boost::asio::ip::tcp;
  class HttpRouter;
  class WebsocketRouter;
  class WebsocketSession;

  // 处理单个HTTP连接的会话
  class HttpSession : public std::enable_shared_from_this<HttpSession>
  {
  public:
    static constexpr std::uint64_t default_max_buffered_request_body_size = 16ULL * 1024 * 1024;

    HttpSession(tcp::socket&& socket, HttpRouter& router, WebsocketRouter& ws_router,
                const std::string& web_root,
                const boost::filesystem::path& canonical_web_root,
                std::uint64_t max_buffered_request_body_size = default_max_buffered_request_body_size);

    // 启动会话
    void run();

  private:
    struct ChunkWriteState;
    class RequestStreamImpl;
    class ResponseStreamImpl;

    bool disable_web_root_ = false;
    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> req_;
    std::optional<http::request_parser<http::buffer_body>> request_parser_;
    std::array<char, 64 * 1024> buffered_body_chunk_{};
    std::string buffered_body_;
    http::response<http::string_body> res_;
    HttpRouter& router_;
    WebsocketRouter& websocket_router_;
    const boost::filesystem::path web_root_path_;
    const boost::filesystem::path canonical_web_root_path_;
    const std::uint64_t max_buffered_request_body_size_;
    std::shared_ptr<WebsocketSession> ws_session_;
    std::optional<http::response_serializer<http::string_body>> sr_;
    http::response<http::buffer_body> streaming_response_;
    std::optional<http::response_serializer<http::buffer_body>> streaming_response_serializer_;
    std::shared_ptr<HttpContext> ctx = nullptr;
    bool stream_completed_ = false;
    std::optional<tcp::endpoint> peer_endpoint_;
    bool request_body_cancelled_ = false;

    // Chunked streaming support
    std::shared_ptr<std::queue<std::string>> chunk_queue_;
    std::shared_ptr<std::mutex> chunk_mtx_;
    std::shared_ptr<bool> chunk_writing_;
    std::shared_ptr<beast::error_code> chunk_error_;

    void do_read();
    void on_read_header(const beast::error_code& ec, std::size_t bytes_transferred);
    void read_buffered_body();
    void on_read_buffered_body(beast::error_code ec, std::size_t bytes_transferred);
    void send_payload_too_large();
    void copy_request_head();
    void handle_stream_request();
    void async_read_stream_body(net::mutable_buffer target, HttpRequestStream::ReadCallback callback);
    void cancel_stream_body();
    void cancel_session();
    void start_stream_response(HttpResponseStream::ResponseHead head, HttpResponseStream::Callback callback);
    void write_stream_response(net::const_buffer source, HttpResponseStream::Callback callback);
    void finish_stream_response(HttpResponseStream::Callback callback);
    void on_read(const beast::error_code& ec, std::size_t bytes_transferred);

    void handle_request();
    void dispatch_request_after_interceptors(InterceptorResult result);
    void send_context_response();
    // 新增：尝试处理静态文件请求
    bool do_serve_static_file();

    void send_chunked_response();
    void send_response(http::message_generator msg);
    void on_write_header(beast::error_code ec, std::size_t bytes_transferred);
    void do_write_chunk(std::shared_ptr<ChunkWriteState> state);
    void on_write(bool keep_alive, beast::error_code ec, std::size_t bytes_transferred);

    void do_write_final_chunk();
    void on_shutdown(bool keep_alive, beast::error_code ec, std::size_t);
    void do_close();

    void handle_websocket_upgrade();
    // 辅助函数：根据文件扩展名获取 MIME 类型
    static std::string mime_type_from_extension(const std::string& ext);
  };
}
#endif // KHTTPD_HTTP_SESSION_HPP
