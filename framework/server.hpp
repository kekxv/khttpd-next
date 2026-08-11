// framework/server.hpp
#ifndef KHTTPD_FRAMEWORK_SERVER_HPP
#define KHTTPD_FRAMEWORK_SERVER_HPP

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/filesystem.hpp>
#include <memory>
#include <optional>
#include <atomic>
#include <cstdint>

#include "router/http_router.hpp"
#include "router/websocket_router.hpp"

namespace khttpd::framework
{
  namespace net = boost::asio;
  using tcp = boost::asio::ip::tcp;

  class Server : public std::enable_shared_from_this<Server>
  {
  public:
    static constexpr std::uint64_t default_max_buffered_request_body_size = 16ULL * 1024 * 1024;

    Server(const tcp::endpoint& endpoint, std::string web_root, int num_threads = 1);

    HttpRouter& get_http_router();
    const HttpRouter& get_http_router() const;

    void add_interceptor(std::shared_ptr<Interceptor> interceptor);

    WebsocketRouter& get_websocket_router();
    const WebsocketRouter& get_websocket_router() const;

    tcp::endpoint local_endpoint() const;

    // Limit for routes that buffer the complete request body in memory.
    // Stream routes are not subject to this limit.
    void set_max_buffered_request_body_size(std::uint64_t bytes);
    std::uint64_t get_max_buffered_request_body_size() const;

    void run();

    void stop();

  private:
    net::signal_set signals_;
    const std::string web_root_;
    boost::filesystem::path canonical_web_root_;

    tcp::acceptor acceptor_;

    HttpRouter http_router_;
    WebsocketRouter websocket_router_;
    std::atomic<std::uint64_t> max_buffered_request_body_size_{default_max_buffered_request_body_size};

    void do_accept();
    void on_accept(boost::beast::error_code ec, tcp::socket socket);
    void handle_signal(const boost::beast::error_code& error, int signal_number);
  };
}
#endif
