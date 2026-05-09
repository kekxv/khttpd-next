// framework/server.cpp
#include "server.hpp"
#include "session/http_session.hpp" // 需要HttpSession
#include <fmt/core.h>
#include <boost/filesystem.hpp>
#include <utility>

#include "io_context_pool.hpp"

namespace khttpd::framework
{
  Server::Server(const tcp::endpoint& endpoint, std::string web_root, int num_threads)
    : signals_(IoContextPool::instance(num_threads).get_io_context(), SIGINT, SIGTERM),
      web_root_(std::move(web_root)),
      acceptor_(net::make_strand(IoContextPool::instance().get_io_context()))
  {
    boost::beast::error_code ec;

    acceptor_.open(endpoint.protocol(), ec);
    if (ec)
    {
      fmt::print(stderr, "Server open error: {}\n", ec.message());
      throw std::runtime_error(fmt::format("Failed to open acceptor: {}", ec.message()));
    }

    acceptor_.set_option(net::socket_base::reuse_address(true), ec);
    if (ec)
    {
      fmt::print(stderr, "Server set_option reuse_address error: {}\n", ec.message());
      throw std::runtime_error(fmt::format("Failed to set reuse_address: {}", ec.message()));
    }

    acceptor_.bind(endpoint, ec);
    if (ec)
    {
      fmt::print(stderr, "Server bind error: {}\n", ec.message());
      throw std::runtime_error(fmt::format("Failed to bind acceptor: {}", ec.message()));
    }

    acceptor_.listen(net::socket_base::max_listen_connections, ec);
    if (ec)
    {
      fmt::print(stderr, "Server listen error: {}\n", ec.message());
      throw std::runtime_error(fmt::format("Failed to listen: {}", ec.message()));
    }

    // Pre-compute canonical web root path once (not per-connection)
    boost::system::error_code path_ec;
    canonical_web_root_ = boost::filesystem::canonical(web_root_, path_ec);
    if (path_ec)
    {
      fmt::print(stderr, "Warning: Cannot canonicalize web_root '{}': {}\n", web_root_, path_ec.message());
    }

    if (!boost::filesystem::exists(web_root_, ec))
    {
      fmt::print(stderr, "Warning: Web root directory '{}' does not exist. Static file serving may fail. Error: {}\n",
                 web_root_, ec.message());
    }
    else if (!boost::filesystem::is_directory(web_root_, ec))
    {
      fmt::print(stderr, "Warning: Web root path '{}' is not a directory. Static file serving may fail. Error: {}\n",
                 web_root_, ec.message());
    }
  }

  HttpRouter& Server::get_http_router()
  {
    return http_router_;
  }

  const HttpRouter& Server::get_http_router() const
  {
    return http_router_;
  }

  void Server::add_interceptor(std::shared_ptr<Interceptor> interceptor)
  {
    http_router_.add_interceptor(interceptor);
  }

  WebsocketRouter& Server::get_websocket_router()
  {
    return websocket_router_;
  }

  const WebsocketRouter& Server::get_websocket_router() const
  {
    return websocket_router_;
  }

  void Server::run()
  {
    fmt::print("Server listening on {}:{}\n", acceptor_.local_endpoint().address().to_string(),
               acceptor_.local_endpoint().port());

    signals_.async_wait(beast::bind_front_handler(&Server::handle_signal, shared_from_this()));

    do_accept();

    IoContextPool::instance().get_io_context().run();

    fmt::print("Server workers stopped.\n");
  }

  void Server::stop()
  {
    boost::beast::error_code ec;
    acceptor_.close(ec);
    if (ec)
    {
      fmt::print(stderr, "Server acceptor close error: {}\n", ec.message());
    }

    IoContextPool::instance().stop();
    fmt::print("Server stopped.\n");
  }

  void Server::do_accept()
  {
    acceptor_.async_accept(
      net::make_strand(IoContextPool::instance().get_io_context()),
      beast::bind_front_handler(&Server::on_accept, shared_from_this()));
  }

  void Server::on_accept(boost::beast::error_code ec, tcp::socket socket)
  {
    if (ec)
    {
      if (ec != boost::system::errc::operation_canceled)
      {
        fmt::print(stderr, "Server on_accept error: {}\n", ec.message());
      }
    }
    else
    {
      std::make_shared<HttpSession>(std::move(socket), http_router_, websocket_router_, web_root_, canonical_web_root_)->run();
    }

    if (acceptor_.is_open())
    {
      do_accept();
    }
  }

  void Server::handle_signal(const boost::beast::error_code& error, int signal_number)
  {
    if (!error)
    {
      fmt::print("Received signal {}, shutting down gracefully...\n", signal_number);
      stop();
    }
  }
} // namespace khttpd::framework
