#include "framework/server.hpp"
#include "framework/context/http_context.hpp"

#include <gtest/gtest.h>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>
#include <vector>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;
namespace fs = boost::filesystem;
namespace khttpd_fw = khttpd::framework;

namespace
{
  constexpr int kThreadCount = 8;
  constexpr int kRequestsPerThread = 50;

  struct TempWebRoot
  {
    fs::path path;

    TempWebRoot()
      : path(fs::temp_directory_path() / fs::unique_path("khttpd-server-stability-%%%%-%%%%-%%%%"))
    {
      fs::create_directories(path);
      std::ofstream((path / "index.html").string()) << "ok";
    }

    ~TempWebRoot()
    {
      boost::system::error_code ignored;
      fs::remove_all(path, ignored);
    }
  };

  bool request_once(unsigned short port, int request_id)
  {
    try
    {
      net::io_context ioc;
      beast::tcp_stream stream(ioc);
      stream.expires_after(std::chrono::seconds(5));
      stream.connect(tcp::endpoint(net::ip::address_v4::loopback(), port));

      http::request<http::string_body> req{http::verb::get, "/ping?id=" + std::to_string(request_id), 11};
      req.set(http::field::host, "127.0.0.1");
      req.set(http::field::user_agent, "khttpd-stability-test");
      req.keep_alive(false);

      http::write(stream, req);

      beast::flat_buffer buffer;
      http::response<http::string_body> res;
      http::read(stream, buffer, res);

      beast::error_code ignored;
      stream.socket().shutdown(tcp::socket::shutdown_both, ignored);

      return res.result() == http::status::ok && res.body() == "pong";
    }
    catch (...)
    {
      return false;
    }
  }
}

TEST(ServerStabilityTest, HandlesManyConcurrentRequests)
{
  TempWebRoot web_root;
  auto server = std::make_shared<khttpd_fw::Server>(
    tcp::endpoint(tcp::v4(), 0),
    web_root.path.string(),
    4);

  std::atomic<int> handled{0};
  server->get_http_router().get("/ping", [&handled](khttpd_fw::HttpContext& ctx)
  {
    handled.fetch_add(1, std::memory_order_relaxed);
    ctx.set_status(http::status::ok);
    ctx.set_body("pong");
    ctx.set_content_type("text/plain");
  });

  const auto port = server->local_endpoint().port();
  std::thread server_thread([server]()
  {
    server->run();
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::atomic<int> successes{0};
  std::vector<std::thread> clients;
  clients.reserve(kThreadCount);

  for (int t = 0; t < kThreadCount; ++t)
  {
    clients.emplace_back([port, t, &successes]()
    {
      for (int i = 0; i < kRequestsPerThread; ++i)
      {
        if (request_once(port, t * kRequestsPerThread + i))
        {
          successes.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (auto& client : clients)
  {
    client.join();
  }

  server->stop();
  if (server_thread.joinable())
  {
    server_thread.join();
  }

  constexpr int total_requests = kThreadCount * kRequestsPerThread;
  EXPECT_EQ(successes.load(), total_requests);
  EXPECT_EQ(handled.load(), total_requests);
}
