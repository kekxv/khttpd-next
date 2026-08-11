#ifndef KHTTPD_FRAMEWORK_TESTS_HTTP_SESSION_TEST_HARNESS_HPP
#define KHTTPD_FRAMEWORK_TESTS_HTTP_SESSION_TEST_HARNESS_HPP

#include "framework/router/http_router.hpp"
#include "framework/router/websocket_router.hpp"
#include "framework/session/http_session.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/filesystem.hpp>
#include <cstdint>
#include <thread>

namespace khttpd::framework::tests
{
  namespace beast = boost::beast;
  namespace http = beast::http;
  namespace net = boost::asio;
  namespace fs = boost::filesystem;
  using tcp = net::ip::tcp;

  struct TempWebRoot
  {
    fs::path path = fs::temp_directory_path() / fs::unique_path("khttpd-http-test-%%%%-%%%%");

    TempWebRoot() { fs::create_directories(path); }
    ~TempWebRoot()
    {
      boost::system::error_code ec;
      fs::remove_all(path, ec);
    }
  };

  template <class Body = http::string_body>
  http::response<Body> round_trip(HttpRouter& router, WebsocketRouter& websocket_router,
                                  const fs::path& web_root,
                                  http::request<http::string_body> request,
                                  std::uint64_t max_buffered_body =
                                    HttpSession::default_max_buffered_request_body_size)
  {
    net::io_context server_ioc;
    auto guard = net::make_work_guard(server_ioc);
    tcp::acceptor acceptor(server_ioc, {net::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = acceptor.local_endpoint();
    const auto canonical_web_root = fs::canonical(web_root);

    acceptor.async_accept([&](beast::error_code ec, tcp::socket socket)
    {
      if (ec) return;
      std::make_shared<HttpSession>(std::move(socket), router, websocket_router,
                                    web_root.string(), canonical_web_root,
                                    max_buffered_body)->run();
    });
    std::thread server_thread([&] { server_ioc.run(); });

    net::io_context client_ioc;
    tcp::socket client(client_ioc);
    client.connect(endpoint);
    beast::error_code ec;
    http::write(client, request, ec);
    if (ec) throw boost::system::system_error(ec);

    beast::flat_buffer buffer;
    http::response_parser<Body> parser;
    http::read(client, buffer, parser, ec);
    if (ec) throw boost::system::system_error(ec);
    auto response = parser.release();

    beast::error_code ignored;
    client.close(ignored);
    guard.reset();
    server_ioc.stop();
    server_thread.join();
    return response;
  }
}

#endif
