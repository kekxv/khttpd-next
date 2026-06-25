#include "framework/session/http_session.hpp"
#include "framework/router/http_router.hpp"
#include "framework/router/websocket_router.hpp"
#include "framework/context/http_context.hpp"

#include <gtest/gtest.h>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/filesystem.hpp>
#include <chrono>
#include <fstream>
#include <atomic>
#include <thread>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;
namespace fs = boost::filesystem;
namespace khttpd_fw = khttpd::framework;

namespace
{
  struct TempStaticTree
  {
    fs::path base;
    fs::path web;
    fs::path web_evil;

    TempStaticTree()
    {
      base = fs::temp_directory_path() / fs::unique_path("khttpd-session-test-%%%%-%%%%-%%%%");
      web = base / "web";
      web_evil = base / "web_evil";
      fs::create_directories(web);
      fs::create_directories(web_evil);
    }

    ~TempStaticTree()
    {
      boost::system::error_code ec;
      fs::remove_all(base, ec);
    }
  };

  template <class Body>
  http::response<Body> round_trip(khttpd_fw::HttpRouter& router,
                                  khttpd_fw::WebsocketRouter& websocket_router,
                                  const fs::path& web_root,
                                  http::request<http::string_body> req,
                                  bool skip_body = false)
  {
    net::io_context ioc;
    tcp::acceptor acceptor(ioc, {net::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = acceptor.local_endpoint();
    const auto canonical_web_root = fs::canonical(web_root);

    acceptor.async_accept([&](beast::error_code ec, tcp::socket socket)
    {
      ASSERT_FALSE(ec) << ec.message();
      std::make_shared<khttpd_fw::HttpSession>(
        std::move(socket), router, websocket_router, web_root.string(), canonical_web_root)->run();
    });

    std::thread server_thread([&]
    {
      ioc.run();
    });

    net::io_context client_ioc;
    tcp::socket client(client_ioc);
    client.connect(endpoint);
    http::write(client, req);

    beast::flat_buffer buffer;
    http::response_parser<Body> parser;
    parser.skip(skip_body);
    http::read(client, buffer, parser);
    auto res = parser.release();

    beast::error_code ignored;
    client.shutdown(tcp::socket::shutdown_both, ignored);
    client.close(ignored);
    ioc.stop();
    server_thread.join();

    return res;
  }
}

TEST(HttpSessionTest, StaticFileRejectsSiblingPrefixTraversal)
{
  TempStaticTree tree;
  std::ofstream((tree.web / "index.txt").string()) << "public";
  std::ofstream((tree.web_evil / "secret.txt").string()) << "secret";

  khttpd_fw::HttpRouter router;
  khttpd_fw::WebsocketRouter websocket_router;
  http::request<http::string_body> req{http::verb::get, "/../web_evil/secret.txt", 11};
  req.keep_alive(false);

  auto res = round_trip<http::string_body>(router, websocket_router, tree.web, std::move(req));

  EXPECT_EQ(res.result(), http::status::forbidden);
  EXPECT_EQ(res.body().find("secret"), std::string::npos);
}

TEST(HttpSessionTest, StaticHeadReturnsHeadersWithoutBody)
{
  TempStaticTree tree;
  std::ofstream((tree.web / "index.txt").string()) << "hello";

  khttpd_fw::HttpRouter router;
  khttpd_fw::WebsocketRouter websocket_router;
  http::request<http::string_body> req{http::verb::head, "/index.txt", 11};
  req.keep_alive(false);

  auto res = round_trip<http::empty_body>(router, websocket_router, tree.web, std::move(req), true);

  EXPECT_EQ(res.result(), http::status::ok);
  ASSERT_TRUE(res.has_content_length());
  EXPECT_EQ(res[http::field::content_length], "5");
}

TEST(HttpSessionTest, ChunkedResponseCompletesWithSingleIoThread)
{
  TempStaticTree tree;

  khttpd_fw::HttpRouter router;
  khttpd_fw::WebsocketRouter websocket_router;
  router.get("/stream", [](khttpd_fw::HttpContext& ctx)
  {
    ctx.set_content_type("text/plain");
    ctx.chunked([](khttpd_fw::HttpContext&, const khttpd_fw::HttpContext::WriteHandler& write)
    {
      ASSERT_TRUE(write("one"));
      ASSERT_TRUE(write("two"));
    });
  });

  http::request<http::string_body> req{http::verb::get, "/stream", 11};
  req.keep_alive(false);

  auto start = std::chrono::steady_clock::now();
  auto res = round_trip<http::string_body>(router, websocket_router, tree.web, std::move(req));
  auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_LT(elapsed, std::chrono::seconds(2));
  EXPECT_EQ(res.result(), http::status::ok);
  EXPECT_TRUE(res.chunked());
  EXPECT_EQ(res.body(), "onetwo");
}

TEST(HttpSessionTest, WebSocketDrainsQueuedMessages)
{
  TempStaticTree tree;
  net::io_context server_ioc;
  khttpd_fw::HttpRouter router;
  khttpd_fw::WebsocketRouter websocket_router;
  std::atomic<bool> closed{false};

  websocket_router.add_handler(
    "/ws",
    nullptr,
    [](khttpd_fw::WebsocketContext& ctx)
    {
      ctx.send("first");
      ctx.send("second");
    },
    [&closed](khttpd_fw::WebsocketContext&)
    {
      closed = true;
    },
    nullptr);

  tcp::acceptor acceptor(server_ioc, {net::ip::make_address("127.0.0.1"), 0});
  const auto endpoint = acceptor.local_endpoint();
  const auto canonical_web_root = fs::canonical(tree.web);

  acceptor.async_accept([&](beast::error_code ec, tcp::socket socket)
  {
    ASSERT_FALSE(ec) << ec.message();
    std::make_shared<khttpd_fw::HttpSession>(
      std::move(socket), router, websocket_router, tree.web.string(), canonical_web_root)->run();
  });

  std::thread server_thread([&]
  {
    server_ioc.run();
  });

  net::io_context client_ioc;
  websocket::stream<tcp::socket> client(client_ioc);
  client.next_layer().connect(endpoint);
  client.handshake("127.0.0.1", "/ws");
  client.write(net::buffer(std::string("go")));

  beast::flat_buffer first_buffer;
  client.read(first_buffer);
  const auto first = beast::buffers_to_string(first_buffer.data());

  beast::flat_buffer second_buffer;
  client.read(second_buffer);
  const auto second = beast::buffers_to_string(second_buffer.data());

  beast::error_code ignored;
  client.close(websocket::close_code::normal, ignored);
  for (int i = 0; i < 100 && !closed; ++i)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  server_ioc.stop();
  server_thread.join();

  EXPECT_TRUE(closed);
  EXPECT_EQ(first, "first");
  EXPECT_EQ(second, "second");
}
