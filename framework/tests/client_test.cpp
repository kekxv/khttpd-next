#include "framework/client/http_client.hpp"
#include "framework/client/websocket_client.hpp"
#include "framework/client/api_macros.hpp"
#include "framework/client/host_pool.hpp"
#include <gtest/gtest.h>
#include <boost/json.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/websocket.hpp>
#include <map>
#include <thread>
#include <atomic>
#include <array>
#include <sstream>
#include <mutex>
#include <vector>
#include <spdlog/spdlog.h>

#include "io_context_pool.hpp"

using namespace khttpd::framework::client;
namespace http = boost::beast::http;
namespace beast = boost::beast;
namespace websocket = boost::beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace
{
class ClientTestLogger : public ::testing::EmptyTestEventListener
{
public:
  void OnTestStart(const ::testing::TestInfo& test_info) override
  {
    spdlog::info("[client_test] START {}.{}", test_info.test_suite_name(), test_info.name());
  }

  void OnTestEnd(const ::testing::TestInfo& test_info) override
  {
    spdlog::info("[client_test] END {}.{} result={}", test_info.test_suite_name(), test_info.name(),
                 test_info.result()->Passed() ? "PASS" : "FAIL");
  }
};

class ClientTestLoggerEnvironment : public ::testing::Environment
{
public:
  void SetUp() override
  {
    spdlog::set_level(spdlog::level::info);
    ::testing::UnitTest::GetInstance()->listeners().Append(new ClientTestLogger());
    spdlog::info("[client_test] logger installed");
  }
};

const auto* const kClientTestLoggerEnvironment =
  ::testing::AddGlobalTestEnvironment(new ClientTestLoggerEnvironment());

class LocalHttpEchoServer
{
public:
  LocalHttpEchoServer()
    : acceptor_(ioc_, tcp::endpoint(net::ip::address_v4::loopback(), 0)),
      port_(acceptor_.local_endpoint().port())
  {
    do_accept();
    thread_ = std::thread([this]() { ioc_.run(); });
  }

  ~LocalHttpEchoServer()
  {
    spdlog::info("[client_test] LocalHttpEchoServer stopping");
    boost::system::error_code ignored;
    acceptor_.close(ignored);
    ioc_.stop();
    if (thread_.joinable()) thread_.join();
    std::vector<std::thread> workers;
    {
      std::lock_guard<std::mutex> lock(workers_mutex_);
      workers.swap(workers_);
    }
    for (auto& worker : workers)
    {
      if (worker.joinable()) worker.join();
    }
    spdlog::info("[client_test] LocalHttpEchoServer stopped");
  }

  std::string base_url() const
  {
    return "http://127.0.0.1:" + std::to_string(port_);
  }

private:
  void do_accept()
  {
    acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket socket)
    {
      if (!ec)
      {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        workers_.emplace_back([socket = std::move(socket)]() mutable
        {
          handle_session(std::move(socket));
        });
      }

      if (acceptor_.is_open())
      {
        do_accept();
      }
    });
  }

  static std::string query_value(std::string target, const std::string& key)
  {
    const auto query_pos = target.find('?');
    if (query_pos == std::string::npos) return "";
    std::string query = target.substr(query_pos + 1);
    std::istringstream parts(query);
    std::string item;
    while (std::getline(parts, item, '&'))
    {
      const auto eq_pos = item.find('=');
      if (eq_pos != std::string::npos && item.substr(0, eq_pos) == key)
      {
        return item.substr(eq_pos + 1);
      }
    }
    return "";
  }

  static void handle_session(tcp::socket socket)
  {
    beast::flat_buffer buffer;
    boost::system::error_code ec;
    http::request<http::string_body> req;
    http::read(socket, buffer, req, ec);
    if (ec) return;

    http::response<http::string_body> res{http::status::ok, req.version()};
    res.set(http::field::server, "khttpd-local-echo");
    res.set(http::field::content_type, "application/json");
    res.keep_alive(false);

    const std::string target(req.target());
    if (target.rfind("/get", 0) == 0)
    {
      res.body() = "{\"foo\":\"" + query_value(target, "foo") + "\",\"id\":\"" +
        query_value(target, "id") + "\",\"msg\":\"" + query_value(target, "msg") + "\"}";
    }
    else if (target.rfind("/headers", 0) == 0)
    {
      std::string body = "{";
      for (const auto& field : req)
      {
        body += "\"" + std::string(field.name_string()) + "\":\"" + std::string(field.value()) + "\",";
      }
      body += "\"done\":true}";
      res.body() = std::move(body);
    }
    else if (target.rfind("/post", 0) == 0)
    {
      res.body() = "{\"data\":" + req.body() + "}";
    }
    else
    {
      res.body() = "{\"target\":\"" + target + "\"}";
    }

    res.prepare_payload();
    http::write(socket, res, ec);
    socket.shutdown(tcp::socket::shutdown_both, ec);
  }

  net::io_context ioc_;
  tcp::acceptor acceptor_;
  unsigned short port_;
  std::thread thread_;
  std::mutex workers_mutex_;
  std::vector<std::thread> workers_;
};

class LocalWebSocketEchoServer
{
public:
  LocalWebSocketEchoServer()
    : acceptor_(ioc_, tcp::endpoint(net::ip::address_v4::loopback(), 0)),
      port_(acceptor_.local_endpoint().port()),
      thread_([this]() { run(); })
  {
  }

  ~LocalWebSocketEchoServer()
  {
    spdlog::info("[client_test] LocalWebSocketEchoServer stopping");
    boost::system::error_code ignored;
    acceptor_.close(ignored);
    ioc_.stop();
    if (thread_.joinable()) thread_.join();
    spdlog::info("[client_test] LocalWebSocketEchoServer stopped");
  }

  std::string url() const
  {
    return "ws://127.0.0.1:" + std::to_string(port_);
  }

private:
  void run()
  {
    boost::system::error_code ec;
    tcp::socket socket(ioc_);
    acceptor_.accept(socket, ec);
    if (ec) return;

    websocket::stream<tcp::socket> ws(std::move(socket));
    ws.accept(ec);
    if (ec) return;

    for (int i = 0; i < 5; ++i)
    {
      beast::flat_buffer buffer;
      ws.read(buffer, ec);
      if (ec) break;
      ws.text(ws.got_text());
      ws.write(buffer.data(), ec);
      if (ec) break;
    }
    ws.close(websocket::close_code::normal, ec);
  }

  net::io_context ioc_;
  tcp::acceptor acceptor_;
  unsigned short port_;
  std::thread thread_;
};

LocalHttpEchoServer& local_http_echo_server()
{
  static LocalHttpEchoServer server;
  return server;
}
}

// ==========================================
// 1. 定义 PostmanEchoClient 类
// ==========================================
class PostmanEchoClient : public HttpClient
{
public:
  // 构造函数：注入 ioc，并设置默认 Base URL
  PostmanEchoClient()
  {
    set_base_url(local_http_echo_server().base_url());
    // 设置一个较长的超时时间，防止 CI 环境网络慢
    set_timeout(std::chrono::seconds(10));
  }

  // ------------------------------------------------------------------
  // API 定义
  // ------------------------------------------------------------------

  // 1. GET 请求，带查询参数
  // Endpoint: /get?foo=bar
  API_CALL(http::verb::get, "/get", echo_get,
           QUERY(std::string, foo_val, "foo"),
           QUERY(int, id_val, "id"))

  // 2. POST 请求，带 JSON Body
  // Endpoint: /post
  API_CALL(http::verb::post, "/post", echo_post,
           BODY(boost::json::object, json_body))

  // 3. GET 请求，测试 Header 传递
  // Endpoint: /headers
  // 我们定义一个名为 request_id 的参数，它会被映射为 HTTP Header "X-Request-Id"
  API_CALL(http::verb::get, "/headers", echo_headers,
           HEADER(std::string, request_id, "X-My-Request-Id"),
           HEADER(std::string, user_token, "X-User-Token"))

  // 4. PUT 请求，带路径参数
  // Endpoint: /put (Postman echo 实际上忽略路径后的东西，但我们可以测试 URL 拼接)
  API_CALL(http::verb::put, "/put", echo_put_dummy)
};

// ==========================================
// 2. 测试用例
// ==========================================

class ClientTest : public ::testing::Test
{
protected:
  boost::asio::io_context ioc;
  std::shared_ptr<PostmanEchoClient> client;

  // 辅助：用于在主线程等待异步结果
  void run_until_complete()
  {
    ioc.run();
    ioc.restart(); // 重置以便下次使用
  }

  void SetUp() override
  {
    client = std::make_shared<PostmanEchoClient>();
  }
};


// 辅助宏：等待异步结果
// 如果 5 秒没结果，这就认为超时失败
#define WAIT_FOR_ASYNC(future) \
    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready) << "Async operation timed out";

TEST_F(ClientTest, GetWithQueryParams)
{
  // 创建一个 promise 用于通知主线程任务完成
  std::promise<void> promise;
  auto future = promise.get_future();

  client->echo_get("hello", 123, [&](auto ec, auto res)
  {
    // 这里的代码在后台线程运行
    if (!ec)
    {
      EXPECT_EQ(res.result(), http::status::ok);
      std::string body = res.body();
      EXPECT_TRUE(body.find("\"foo\":\"hello\"") != std::string::npos);
      EXPECT_TRUE(body.find("\"id\":\"123\"") != std::string::npos);
    }
    else
    {
      ADD_FAILURE() << "Network error: " << ec.message();
    }

    // 通知主线程：我做完了
    promise.set_value();
  });

  // 主线程在此阻塞等待，直到 callback 执行完毕
  WAIT_FOR_ASYNC(future);
}

TEST_F(ClientTest, PostJsonBody)
{
  std::promise<void> promise;
  auto future = promise.get_future();

  boost::json::object jv;
  jv["message"] = "test_payload";
  jv["count"] = 99;

  client->echo_post(jv, [&](auto ec, auto res)
  {
    if (!ec)
    {
      EXPECT_EQ(res.result(), http::status::ok);
      std::string body = res.body();
      EXPECT_TRUE(body.find("test_payload") != std::string::npos);
    }
    else
    {
      ADD_FAILURE() << "Network error: " << ec.message();
    }
    promise.set_value();
  });

  WAIT_FOR_ASYNC(future);
}

TEST_F(ClientTest, CustomHeaders)
{
  std::promise<void> promise;
  auto future = promise.get_future();

  std::string rid = "req-unique-id-001";
  std::string token = "secret-token-abc";

  client->echo_headers(rid, token, [&](auto ec, auto res)
  {
    if (!ec)
    {
      EXPECT_EQ(res.result(), http::status::ok);
      std::string body = res.body();

      bool has_rid = body.find("x-my-request-id") != std::string::npos ||
        body.find("X-My-Request-Id") != std::string::npos;
      bool has_val = body.find(rid) != std::string::npos;

      EXPECT_TRUE(has_rid) << "Missing Header Key";
      EXPECT_TRUE(has_val) << "Missing Header Value";
    }
    else
    {
      ADD_FAILURE() << "Network error: " << ec.message();
    }
    promise.set_value();
  });

  WAIT_FOR_ASYNC(future);
}

TEST_F(ClientTest, GlobalDefaultHeader)
{
  std::promise<void> promise;
  auto future = promise.get_future();

  client->set_default_header("X-App-Version", "v1.0.0-beta");

  client->echo_headers("id-1", "token-1", [&](auto ec, auto res)
  {
    if (!ec)
    {
      std::string body = res.body();
      EXPECT_TRUE(body.find("v1.0.0-beta") != std::string::npos);
    }
    promise.set_value();
  });

  WAIT_FOR_ASYNC(future);
}

// 同步调用测试 (现在非常安全，不会死锁)
TEST_F(ClientTest, SyncCallSafe)
{
  try
  {
    // 主线程调用，后台线程执行，future wait 自动处理
    auto res = client->echo_get_sync("sync_world", 999);

    EXPECT_EQ(res.result(), http::status::ok);
    std::string body = res.body();
    EXPECT_TRUE(body.find("sync_world") != std::string::npos);
  }
  catch (const std::exception& e)
  {
    ADD_FAILURE() << "Sync request exception: " << e.what();
  }
}

TEST_F(ClientTest, SyncCall)
{
  // 重要：同步调用会阻塞当前线程等待 future，
  // 所以 io_context 必须在另一个线程跑，否则死锁。
  auto work = boost::asio::make_work_guard(ioc);
  std::thread ioc_thread([&]
  {
    ioc.run();
  });

  try
  {
    // 使用同步生成的 API
    auto res = client->echo_get_sync("sync_world", 999);

    EXPECT_EQ(res.result(), http::status::ok);
    std::string body = res.body();
    EXPECT_TRUE(body.find("sync_world") != std::string::npos);
  }
  catch (const std::exception& e)
  {
    ADD_FAILURE() << "Sync request exception: " << e.what();
  }

  // 清理
  work.reset();
  ioc.stop();
  if (ioc_thread.joinable()) ioc_thread.join();
}

TEST(EasyModeTest, SyncRequestWithoutManualContext)
{
  // 不需要手动创建 ioc, work_guard, thread
  auto client = std::make_shared<PostmanEchoClient>(); // 使用默认构造

  try
  {
    // 直接调用同步接口
    auto res = client->echo_get_sync("easy_mode", 1);
    EXPECT_EQ(res.result(), http::status::ok);
    EXPECT_TRUE(res.body().find("easy_mode") != std::string::npos);
  }
  catch (const std::exception& e)
  {
    ADD_FAILURE() << "Exception: " << e.what();
  }
}

TEST(EasyModeTest, AsyncRequest)
{
  auto client = std::make_shared<PostmanEchoClient>();

  std::promise<void> done;
  auto future = done.get_future();

  client->echo_get("async_easy", 2, [&](auto ec, auto res)
  {
    EXPECT_FALSE(ec);
    done.set_value();
  });

  // 等待异步结果
  // 因为 ioc 在后台线程跑，这里我们需要 wait
  future.wait();
}

TEST(HttpClientLocalTest, SyncRequestWithUnrunExternalIoContextTimesOut)
{
  boost::asio::io_context ioc;
  HttpClient client(ioc);
  client.set_base_url("http://127.0.0.1:9");
  client.set_timeout(std::chrono::seconds(0));

  auto start = std::chrono::steady_clock::now();
  EXPECT_THROW(
    client.request_sync(http::verb::get, "/", {}, "", {}),
    boost::system::system_error);
  auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_LT(elapsed, std::chrono::seconds(2));
}

TEST(HttpClientLocalTest, SyncRequestTimeoutClosesStalledConnection)
{
  boost::asio::io_context server_ioc;
  tcp::acceptor acceptor(server_ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
  const auto endpoint = acceptor.local_endpoint();
  std::atomic<bool> accepted{false};
  std::atomic<bool> client_closed{false};
  auto server_socket = std::make_shared<tcp::socket>(server_ioc);
  auto read_buffer = std::make_shared<std::array<char, 1024>>();
  auto read_until_close = std::make_shared<std::function<void()>>();
  *read_until_close = [server_socket, read_buffer, read_until_close, &client_closed]()
  {
    server_socket->async_read_some(boost::asio::buffer(*read_buffer),
                                   [read_until_close, &client_closed](boost::system::error_code read_ec,
                                                                      std::size_t)
                                   {
                                     if (read_ec)
                                     {
                                       client_closed = true;
                                       return;
                                     }
                                     (*read_until_close)();
                                   });
  };

  acceptor.async_accept(*server_socket, [&](boost::system::error_code ec)
  {
    ASSERT_FALSE(ec) << ec.message();
    accepted = true;
    (*read_until_close)();
  });

  std::thread server_thread([&] { server_ioc.run(); });

  boost::asio::io_context client_ioc;
  auto work = boost::asio::make_work_guard(client_ioc);
  std::thread client_thread([&] { client_ioc.run(); });

  HttpClient client(client_ioc);
  client.set_base_url("http://127.0.0.1:" + std::to_string(endpoint.port()));
  client.set_timeout(std::chrono::seconds(1));

  EXPECT_THROW(client.request_sync(http::verb::get, "/", {}, "", {}), boost::system::system_error);

  for (int i = 0; i < 100 && (!accepted || !client_closed); ++i)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  work.reset();
  client_ioc.stop();
  server_ioc.stop();
  if (client_thread.joinable()) client_thread.join();
  if (server_thread.joinable()) server_thread.join();

  EXPECT_TRUE(accepted);
  EXPECT_TRUE(client_closed);
}


// ==========================================
// WebSocket 测试
// ==========================================

class WebsocketTest : public ::testing::Test
{
protected:
  boost::asio::io_context ioc;
  std::shared_ptr<WebsocketClient> ws_client;

  void SetUp() override
  {
    ws_client = std::make_shared<WebsocketClient>(ioc);
  }

  void TearDown() override
  {
    if (ws_client) ws_client->close();
  }
};

TEST_F(WebsocketTest, WssEchoAndWriteQueue)
{
  LocalWebSocketEchoServer server;
  std::string url = server.url();

  const int message_count = 5;
  int received_count = 0;
  bool closed_gracefully = false;

  // 增加一个 flag 标记是否发生严重错误
  bool has_error = false;

  // 创建定时器，但先不 async_wait，后面逻辑控制
  boost::asio::steady_timer timer(ioc, std::chrono::seconds(15));

  ws_client->set_on_message([&](const std::string& msg)
  {
    // 过滤欢迎消息
    if (msg.find("Request served by") != std::string::npos) return;

    received_count++;
    // spdlog::debug("Msg: {}", msg);

    if (received_count >= message_count)
    {
      ws_client->close();
    }
  });

  ws_client->set_on_close([&]()
  {
    closed_gracefully = true;
    // 关键：连接关闭后，取消定时器，ioc.run() 就会立即返回
    timer.cancel();
  });

  ws_client->set_on_error([&](boost::beast::error_code ec)
  {
    // 忽略操作取消（通常是 close() 导致的 pending read 取消）
    if (ec == boost::asio::error::operation_aborted) return;

    spdlog::error("WS Error: {}", ec.message());
    has_error = true;
    timer.cancel(); // 发生错误也停止测试
  });

  ws_client->connect(url, [&](boost::beast::error_code ec)
  {
    if (ec)
    {
      ADD_FAILURE() << "WS Connect Failed: " << ec.message();
      timer.cancel();
      return;
    }

    for (int i = 0; i < message_count; ++i)
    {
      ws_client->send("Msg-" + std::to_string(i));
    }
  });

  // 启动超时计时
  timer.async_wait([&](boost::system::error_code ec)
  {
    if (ec == boost::asio::error::operation_aborted)
    {
      // 定时器被取消，说明测试正常结束或提前出错
      return;
    }
    // 定时器真的触发了 -> 超时
    ws_client->close();
    ADD_FAILURE() << "Test Timed Out! Received: " << received_count << "/" << message_count;
  });

  ioc.run();

  EXPECT_FALSE(has_error) << "Should not encounter network errors";
  EXPECT_EQ(received_count, message_count);
  EXPECT_TRUE(closed_gracefully) << "on_close should be triggered";
}

TEST_F(WebsocketTest, ConnectFailure)
{
  // 测试连接不可达端口
  bool failed = false;
  ws_client->connect("ws://localhost:59999", [&](boost::beast::error_code ec)
  {
    if (ec)
    {
      failed = true;
    }
  });

  ioc.run();
  EXPECT_TRUE(failed);
}

TEST_F(WebsocketTest, CloseBeforeHandshakeSuppressesConnectCallback)
{
  boost::asio::io_context server_ioc;
  tcp::acceptor acceptor(server_ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
  const auto endpoint = acceptor.local_endpoint();
  auto server_socket = std::make_shared<tcp::socket>(server_ioc);

  acceptor.async_accept(*server_socket, [](boost::system::error_code) {});
  std::thread server_thread([&] { server_ioc.run(); });

  std::atomic<int> connect_calls{0};
  ws_client->connect("ws://127.0.0.1:" + std::to_string(endpoint.port()), [&](boost::beast::error_code)
  {
    ++connect_calls;
  });
  ws_client->close();

  boost::asio::steady_timer timer(ioc, std::chrono::milliseconds(300));
  timer.async_wait([&](boost::system::error_code) { ioc.stop(); });
  ioc.run();

  server_ioc.stop();
  if (server_thread.joinable()) server_thread.join();

  EXPECT_EQ(connect_calls.load(), 0);
}

TEST(WebsocketClientLifecycleTest, DestructorSuppressesPendingConnectCallback)
{
  boost::asio::io_context server_ioc;
  tcp::acceptor acceptor(server_ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
  const auto endpoint = acceptor.local_endpoint();
  auto server_socket = std::make_shared<tcp::socket>(server_ioc);

  acceptor.async_accept(*server_socket, [](boost::system::error_code) {});
  std::thread server_thread([&] { server_ioc.run(); });

  boost::asio::io_context client_ioc;
  std::atomic<int> connect_calls{0};
  {
    auto client = std::make_shared<WebsocketClient>(client_ioc);
    client->connect("ws://127.0.0.1:" + std::to_string(endpoint.port()), [&](boost::beast::error_code)
    {
      ++connect_calls;
    });
  }

  boost::asio::steady_timer timer(client_ioc, std::chrono::milliseconds(300));
  timer.async_wait([&](boost::system::error_code) { client_ioc.stop(); });
  client_ioc.run();

  server_ioc.stop();
  if (server_thread.joinable()) server_thread.join();

  EXPECT_EQ(connect_calls.load(), 0);
}

TEST_F(ClientTest, ThreadPoolVerify)
{
  spdlog::debug("Pool Size: {}", khttpd::framework::IoContextPool::instance().get_thread_count());

  std::promise<void> p1, p2;
  auto f1 = p1.get_future();
  auto f2 = p2.get_future();

  // 发起两个请求
  client->echo_get("A", 1, [&](auto, auto)
  {
    spdlog::debug("Req 1 processed on thread hash: {}", std::hash<std::thread::id>{}(std::this_thread::get_id()));
    p1.set_value();
  });

  client->echo_get("B", 2, [&](auto, auto)
  {
    spdlog::debug("Req 2 processed on thread hash: {}", std::hash<std::thread::id>{}(std::this_thread::get_id()));
    p2.set_value();
  });

  WAIT_FOR_ASYNC(f1);
  WAIT_FOR_ASYNC(f2);
}

// ==========================================
// 3. Oat++-style API Client Tests
// ==========================================

// Define API client using KHTTPD_API_CLIENT (single host, endpoints use API_CALL)
KHTTPD_API_CLIENT(EchoClient, "http://127.0.0.1:1")
    API_CALL(http::verb::get, "/get", get_echo,
             QUERY(std::string, msg, "msg"))
    API_CALL(http::verb::post, "/post", post_echo,
             BODY(boost::json::object, body))
KHTTPD_API_CLIENT_END()

// Define API client using KHTTPD_API_CLIENT_POOL (multi-host with weights)
KHTTPD_API_CLIENT_POOL(MultiHostClient,
    KHTTPD_HOST("http://127.0.0.1:1", 3)
    KHTTPD_HOST("http://127.0.0.1:1", 1)
)
    API_CALL(http::verb::get, "/get", get_echo,
             QUERY(std::string, msg, "msg"))
KHTTPD_API_CLIENT_END()

// Test verb_from_string
TEST(ApiMacrosTest, VerbFromString)
{
  ASSERT_EQ(verb_from_string("GET"), http::verb::get);
  ASSERT_EQ(verb_from_string("get"), http::verb::get);
  ASSERT_EQ(verb_from_string("POST"), http::verb::post);
  ASSERT_EQ(verb_from_string("post"), http::verb::post);
  ASSERT_EQ(verb_from_string("PUT"), http::verb::put);
  ASSERT_EQ(verb_from_string("DELETE"), http::verb::delete_);
  ASSERT_EQ(verb_from_string("PATCH"), http::verb::patch);
  ASSERT_EQ(verb_from_string("HEAD"), http::verb::head);
  ASSERT_EQ(verb_from_string("OPTIONS"), http::verb::options);
  ASSERT_EQ(verb_from_string("UNKNOWN"), http::verb::get); // fallback
}

// Test single-host KHTTPD_API_CLIENT
TEST_F(ClientTest, OatppStyleSingleHost)
{
  auto echo = std::make_shared<EchoClient>();
  echo->set_base_url(local_http_echo_server().base_url());
  echo->set_timeout(std::chrono::seconds(10));

  std::promise<void> done;
  auto future = done.get_future();

  echo->get_echo("hello", [&](auto ec, auto res) {
    if (!ec) {
      EXPECT_EQ(res.result(), http::status::ok);
      EXPECT_TRUE(res.body().find("hello") != std::string::npos);
    } else {
      ADD_FAILURE() << "Network error: " << ec.message();
    }
    done.set_value();
  });

  WAIT_FOR_ASYNC(future);
}

// Test sync version
TEST_F(ClientTest, OatppStyleSync)
{
  auto echo = std::make_shared<EchoClient>();
  echo->set_base_url(local_http_echo_server().base_url());
  echo->set_timeout(std::chrono::seconds(10));

  try {
    auto res = echo->get_echo_sync("sync_test");
    EXPECT_EQ(res.result(), http::status::ok);
    EXPECT_TRUE(res.body().find("sync_test") != std::string::npos);
  } catch (const std::exception& e) {
    ADD_FAILURE() << "Exception: " << e.what();
  }
}

// Test multi-host pool
TEST(ApiMacrosTest, HostPoolWeighted)
{
  std::vector<HostEntry> hosts = {
    {"http://host-a.com", 3},
    {"http://host-b.com", 1},
  };
  HostPool pool(hosts);

  // All URLs should be present
  auto urls = pool.all_urls();
  ASSERT_EQ(urls.size(), 2);
  ASSERT_EQ(urls[0], "http://host-a.com");
  ASSERT_EQ(urls[1], "http://host-b.com");

  // Total weight should be 4
  ASSERT_EQ(pool.total_weight(), 4);

  // pick() should always return one of the hosts
  for (int i = 0; i < 100; ++i) {
    const auto& picked = pool.pick();
    ASSERT_TRUE(picked == "http://host-a.com" || picked == "http://host-b.com");
  }
}

TEST(ApiMacrosTest, HostPoolPickIsThreadSafe)
{
  std::vector<HostEntry> hosts = {
    {"http://host-a.com", 3},
    {"http://host-b.com", 1},
  };
  HostPool pool(hosts);
  std::atomic<int> picks{0};
  std::vector<std::thread> threads;

  for (int t = 0; t < 8; ++t)
  {
    threads.emplace_back([&]()
    {
      for (int i = 0; i < 1000; ++i)
      {
        const auto& picked = pool.pick();
        ASSERT_TRUE(picked == "http://host-a.com" || picked == "http://host-b.com");
        picks++;
      }
    });
  }

  for (auto& thread : threads)
  {
    thread.join();
  }

  ASSERT_EQ(picks.load(), 8000);
}

// Test multi-host API client
TEST_F(ClientTest, MultiHostClientPool)
{
  auto mc = std::make_shared<MultiHostClient>();
  mc->set_base_url(local_http_echo_server().base_url());
  mc->set_timeout(std::chrono::seconds(10));

  std::promise<void> done;
  auto future = done.get_future();

  mc->get_echo("pool_test", [&](auto ec, auto res) {
    if (!ec) {
      EXPECT_EQ(res.result(), http::status::ok);
    } else {
      ADD_FAILURE() << "Network error: " << ec.message();
    }
    done.set_value();
  });

  WAIT_FOR_ASYNC(future);
}
