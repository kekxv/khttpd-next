// framework/server.hpp
#ifndef KHTTPD_FRAMEWORK_SERVER_HPP
#define KHTTPD_FRAMEWORK_SERVER_HPP

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/filesystem.hpp>
#include <memory>
#include <optional>

#include "router/http_router.hpp"
#include "router/websocket_router.hpp"

namespace khttpd::framework
{
  namespace net = boost::asio;
  using tcp = boost::asio::ip::tcp;

  class Server : public std::enable_shared_from_this<Server>
  {
  public:
    Server(const tcp::endpoint& endpoint, std::string web_root, int num_threads = 1);

    HttpRouter& get_http_router();
    const HttpRouter& get_http_router() const;

    void add_interceptor(std::shared_ptr<Interceptor> interceptor);

    WebsocketRouter& get_websocket_router();
    const WebsocketRouter& get_websocket_router() const;

    tcp::endpoint local_endpoint() const;

    void run();

    void stop();

  private:
    net::signal_set signals_;
    const std::string web_root_;
    boost::filesystem::path canonical_web_root_;

    tcp::acceptor acceptor_;

    HttpRouter http_router_;
    WebsocketRouter websocket_router_;

    void do_accept();
    void on_accept(boost::beast::error_code ec, tcp::socket socket);
    void handle_signal(const boost::beast::error_code& error, int signal_number);
  };
}
#endif
