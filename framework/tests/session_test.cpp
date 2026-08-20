#include "framework/session/http_session.hpp"
#include "framework/router/http_router.hpp"
#include "framework/router/websocket_router.hpp"
#include "framework/context/http_context.hpp"
#include "framework/client/http_proxy_session.hpp"

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
                                  bool skip_body = false,
                                  std::function<bool()> completion = {},
                                  std::uint64_t max_buffered_request_body_size =
                                    khttpd_fw::HttpSession::default_max_buffered_request_body_size)
  {
    net::io_context ioc;
    auto work_guard = net::make_work_guard(ioc);
    tcp::acceptor acceptor(ioc, {net::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = acceptor.local_endpoint();
    const auto canonical_web_root = fs::canonical(web_root);

    acceptor.async_accept([&](beast::error_code ec, tcp::socket socket)
    {
      ASSERT_FALSE(ec) << ec.message();
      std::make_shared<khttpd_fw::HttpSession>(
        std::move(socket), router, websocket_router, web_root.string(), canonical_web_root,
        max_buffered_request_body_size)->run();
    });

    std::thread server_thread([&]
    {
      ioc.run();
    });

    net::io_context client_ioc;
    tcp::socket client(client_ioc);
    client.connect(endpoint);
    beast::error_code io_ec;
    http::write(client, req, io_ec);
    EXPECT_FALSE(io_ec) << io_ec.message();

    beast::flat_buffer buffer;
    http::response_parser<Body> parser;
    parser.skip(skip_body);
    http::read(client, buffer, parser, io_ec);
    EXPECT_FALSE(io_ec) << io_ec.message();
    auto res = parser.release();

    for (int i = 0; completion && !completion() && i < 1000; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));

    beast::error_code ignored;
    client.shutdown(tcp::socket::shutdown_both, ignored);
    client.close(ignored);
    work_guard.reset();
    ioc.stop();
    server_thread.join();

    return res;
  }
}

TEST(HttpSessionTest, ConfigurableBufferedBodyLimitAcceptsBodyAtLimit)
{
  TempStaticTree tree;
  khttpd_fw::HttpRouter router;
  khttpd_fw::WebsocketRouter websocket_router;
  constexpr std::uint64_t limit = 1024;
  router.post("/buffered", [](khttpd_fw::HttpContext& ctx)
  {
    ctx.set_body(std::to_string(ctx.get_request().body().size()));
  });

  http::request<http::string_body> req{http::verb::post, "/buffered", 11};
  req.body().assign(limit, 'x');
  req.prepare_payload();
  req.keep_alive(false);

  auto res = round_trip<http::string_body>(router, websocket_router, tree.web, std::move(req),
                                            false, {}, limit);
  EXPECT_EQ(res.result(), http::status::ok);
  EXPECT_EQ(res.body(), std::to_string(limit));
}

TEST(HttpSessionTest, ConfigurableBufferedBodyLimitRejectsContentLengthBeforeHandler)
{
  TempStaticTree tree;
  khttpd_fw::HttpRouter router;
  khttpd_fw::WebsocketRouter websocket_router;
  constexpr std::uint64_t limit = 1024;
  bool handled = false;
  router.post("/buffered", [&handled](khttpd_fw::HttpContext&) { handled = true; });

  http::request<http::string_body> req{http::verb::post, "/buffered", 11};
  req.body().assign(limit + 1, 'x');
  req.prepare_payload();
  req.keep_alive(false);

  auto res = round_trip<http::string_body>(router, websocket_router, tree.web, std::move(req),
                                            false, {}, limit);
  EXPECT_EQ(res.result(), http::status::payload_too_large);
  EXPECT_FALSE(handled);
}

TEST(HttpSessionTest, ConfigurableBufferedBodyLimitRejectsChunkedBodyWhileReading)
{
  TempStaticTree tree;
  khttpd_fw::HttpRouter router;
  khttpd_fw::WebsocketRouter websocket_router;
  constexpr std::uint64_t limit = 1024;
  bool handled = false;
  router.post("/buffered", [&handled](khttpd_fw::HttpContext&) { handled = true; });

  http::request<http::string_body> req{http::verb::post, "/buffered", 11};
  req.body().assign(limit + 1, 'x');
  req.chunked(true);
  req.keep_alive(false);

  auto res = round_trip<http::string_body>(router, websocket_router, tree.web, std::move(req),
                                            false, {}, limit);
  EXPECT_EQ(res.result(), http::status::payload_too_large);
  EXPECT_FALSE(handled);
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

TEST(HttpSessionTest, DynamicHeadUsesGetMetadataWithoutSendingBody)
{
  TempStaticTree tree;
  khttpd_fw::HttpRouter router;
  khttpd_fw::WebsocketRouter websocket_router;
  router.get("/resource", [](khttpd_fw::HttpContext& ctx) { ctx.set_body("generated-body"); });
  http::request<http::string_body> req{http::verb::head, "/resource", 11};
  req.keep_alive(false);
  auto res = round_trip<http::empty_body>(router, websocket_router, tree.web, std::move(req), true);
  EXPECT_EQ(res.result(), http::status::ok);
  EXPECT_EQ(res[http::field::content_length], "14");
}

TEST(HttpSessionTest, EmptyRouteWritesZeroContentLength)
{
  TempStaticTree tree;
  khttpd_fw::HttpRouter router;
  khttpd_fw::WebsocketRouter websocket_router;
  router.get("/empty", [](khttpd_fw::HttpContext&) {});

  http::request<http::string_body> req{http::verb::get, "/empty", 11};
  req.keep_alive(false);
  auto res = round_trip<http::string_body>(router, websocket_router, tree.web, std::move(req));

  EXPECT_EQ(res.result(), http::status::ok);
  ASSERT_TRUE(res.has_content_length());
  EXPECT_EQ(res[http::field::content_length], "0");
  EXPECT_TRUE(res.body().empty());
}

TEST(HttpSessionTest, AsyncInterceptorSeesTransportPeerAndCanDenyRequest)
{
  struct RemoteAuth final : khttpd_fw::Interceptor
  {
    std::optional<tcp::endpoint> seen_peer;
    void async_handle_request(khttpd_fw::HttpContext& ctx, RequestCompletion complete) override
    {
      seen_peer = ctx.peer_endpoint();
      ctx.set_status(http::status::unauthorized);
      ctx.set_body("remote auth denied");
      std::thread([complete = std::move(complete)] { complete(khttpd_fw::InterceptorResult::Stop); }).detach();
    }
  };
  TempStaticTree tree;
  khttpd_fw::HttpRouter router;
  khttpd_fw::WebsocketRouter websocket_router;
  auto auth = std::make_shared<RemoteAuth>();
  router.add_interceptor(auth);
  router.get("/private", [](khttpd_fw::HttpContext& ctx) { ctx.set_body("should not run"); });
  http::request<http::string_body> req{http::verb::get, "/private", 11};
  req.keep_alive(false);
  auto res = round_trip<http::string_body>(router, websocket_router, tree.web, std::move(req));
  EXPECT_EQ(res.result(), http::status::unauthorized);
  EXPECT_EQ(res.body(), "remote auth denied");
  ASSERT_TRUE(auth->seen_peer);
  EXPECT_TRUE(auth->seen_peer->address().is_loopback());
  EXPECT_NE(auth->seen_peer->port(), 0);
}

TEST(HttpSessionTest, TypedRouteCannotBypassAuthorizationInterceptor)
{
  struct DenyAccess final : khttpd_fw::Interceptor
  {
    khttpd_fw::InterceptorResult handle_request(khttpd_fw::HttpContext& ctx) override
    {
      ctx.set_status(http::status::forbidden);
      ctx.set_content_type("application/json");
      ctx.set_body(R"({"code":"FORBIDDEN"})");
      return khttpd_fw::InterceptorResult::Stop;
    }
  };

  TempStaticTree tree;
  khttpd_fw::HttpRouter router;
  khttpd_fw::WebsocketRouter websocket_router;
  int handler_calls = 0;
  router.add_interceptor(std::make_shared<DenyAccess>());
  router.post("/typed-private", [&handler_calls](const boost::json::object& body)
  {
    ++handler_calls;
    return body;
  });

  http::request<http::string_body> req{http::verb::post, "/typed-private", 11};
  req.set(http::field::content_type, "application/json");
  req.body() = R"({"secret":"request"})";
  req.prepare_payload();
  req.keep_alive(false);

  auto res = round_trip<http::string_body>(router, websocket_router, tree.web, std::move(req));

  EXPECT_EQ(res.result(), http::status::forbidden);
  EXPECT_EQ(res.body(), R"({"code":"FORBIDDEN"})");
  EXPECT_EQ(handler_calls, 0);
}

TEST(HttpSessionTest, TypedHandlerExceptionUsesRegisteredMapper)
{
  class SessionValidationError : public std::runtime_error
  {
  public:
    using std::runtime_error::runtime_error;
  };
  class SecurityHeaders final : public khttpd_fw::Interceptor
  {
  public:
    void handle_response(khttpd_fw::HttpContext& ctx) override
    {
      ctx.set_header("X-Content-Type-Options", "nosniff");
    }
  };

  TempStaticTree tree;
  khttpd_fw::HttpRouter router;
  khttpd_fw::WebsocketRouter websocket_router;
  router.add_interceptor(std::make_shared<SecurityHeaders>());
  router.map_exception<SessionValidationError>([](const SessionValidationError& error)
  {
    boost::json::object body;
    body.emplace("code", "SESSION_VALIDATION_FAILED");
    body.emplace("message", error.what());
    return khttpd_fw::HttpResult<boost::json::object>(http::status::unprocessable_entity, std::move(body));
  });
  router.post("/typed-error", [](const boost::json::object&) -> boost::json::object
  {
    throw SessionValidationError("typed request rejected");
  });

  http::request<http::string_body> req{http::verb::post, "/typed-error", 11};
  req.set(http::field::content_type, "application/json");
  req.body() = R"({"value":1})";
  req.prepare_payload();
  req.keep_alive(false);

  auto res = round_trip<http::string_body>(router, websocket_router, tree.web, std::move(req));

  EXPECT_EQ(res.result(), http::status::unprocessable_entity);
  EXPECT_EQ(res[http::field::content_type], "application/json");
  EXPECT_EQ(res["X-Content-Type-Options"], "nosniff");
  EXPECT_EQ(res.body(),
            R"({"code":"SESSION_VALIDATION_FAILED","message":"typed request rejected"})");
}

TEST(HttpSessionTest, ThrowingPostInterceptorRunsOnlyOnceAndReturnsSafeError)
{
  class ThrowingPostInterceptor final : public khttpd_fw::Interceptor
  {
  public:
    int calls = 0;

    void handle_response(khttpd_fw::HttpContext&) override
    {
      ++calls;
      throw std::runtime_error("post interceptor secret");
    }
  };

  TempStaticTree tree;
  khttpd_fw::HttpRouter router;
  khttpd_fw::WebsocketRouter websocket_router;
  auto interceptor = std::make_shared<ThrowingPostInterceptor>();
  router.add_interceptor(interceptor);
  router.get("/post-error", [](khttpd_fw::HttpContext& ctx) { ctx.set_body("success"); });

  http::request<http::string_body> req{http::verb::get, "/post-error", 11};
  req.keep_alive(false);
  auto res = round_trip<http::string_body>(router, websocket_router, tree.web, std::move(req));

  EXPECT_EQ(interceptor->calls, 1);
  EXPECT_EQ(res.result(), http::status::internal_server_error);
  EXPECT_EQ(res.body(), R"({"code":"INTERNAL_SERVER_ERROR","message":"Internal server error"})");
  EXPECT_EQ(res.body().find("post interceptor secret"), std::string::npos);
}

TEST(HttpSessionTest, AsyncRouteCompletesResponseFromAnotherThread)
{
  TempStaticTree tree;
  khttpd_fw::HttpRouter router;
  khttpd_fw::WebsocketRouter websocket_router;
  router.async_route("/remote", http::verb::get,
    [](khttpd_fw::HttpContext& ctx, khttpd_fw::HttpAsyncComplete complete)
    {
      std::thread([&ctx, complete = std::move(complete)]() mutable
      {
        ctx.set_body("async result");
        complete();
      }).detach();
    });
  http::request<http::string_body> req{http::verb::get, "/remote", 11};
  req.keep_alive(false);
  auto res = round_trip<http::string_body>(router, websocket_router, tree.web, std::move(req));
  EXPECT_EQ(res.result(), http::status::ok);
  EXPECT_EQ(res.body(), "async result");
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

TEST(HttpSessionTest, StreamsContentLengthRequestBodyInFixedBuffers)
{
  TempStaticTree tree;
  khttpd_fw::HttpRouter router;
  khttpd_fw::WebsocketRouter websocket_router;
  // Deliberately exceeds the 16 MiB buffered-route limit. A stream route must
  // consume it successfully without accumulating the body in HttpSession.
  constexpr std::size_t body_size = 20 * 1024 * 1024 + 17;

  router.stream("/upload", http::verb::post,
    [](khttpd_fw::HttpContext& ctx, std::shared_ptr<khttpd_fw::HttpRequestStream> stream,
       std::shared_ptr<khttpd_fw::HttpResponseStream>,
       khttpd_fw::HttpStreamComplete complete)
    {
      struct State {
        std::array<char, 32 * 1024> buffer{};
        std::size_t total = 0;
        std::shared_ptr<khttpd_fw::HttpRequestStream> stream;
        khttpd_fw::HttpContext* ctx;
        khttpd_fw::HttpStreamComplete complete;
        std::function<void()> next;
      };
      auto state = std::make_shared<State>();
      state->stream = std::move(stream); state->ctx = &ctx; state->complete = std::move(complete);
      state->next = [state]
      {
        state->stream->async_read_some(net::buffer(state->buffer), [state](beast::error_code ec, std::size_t n, bool done)
        {
          ASSERT_FALSE(ec) << ec.message();
          state->total += n;
          if (!done) return state->next();
          state->ctx->set_body(std::to_string(state->total));
          state->complete();
        });
      };
      state->next();
    });

  http::request<http::string_body> req{http::verb::post, "/upload", 11};
  req.body().assign(body_size, 'x');
  req.prepare_payload();
  req.keep_alive(false);
  auto res = round_trip<http::string_body>(router, websocket_router, tree.web, std::move(req));
  EXPECT_EQ(res.result(), http::status::ok);
  EXPECT_EQ(res.body(), std::to_string(body_size));
}

TEST(HttpSessionTest, StreamsChunkedRequestBody)
{
  TempStaticTree tree;
  khttpd_fw::HttpRouter router;
  khttpd_fw::WebsocketRouter websocket_router;
  router.stream("/chunked", http::verb::post,
    [](khttpd_fw::HttpContext& ctx, std::shared_ptr<khttpd_fw::HttpRequestStream> stream,
       std::shared_ptr<khttpd_fw::HttpResponseStream>,
       khttpd_fw::HttpStreamComplete complete)
    {
      auto buffer = std::make_shared<std::array<char, 7>>();
      auto total = std::make_shared<std::size_t>(0);
      auto next = std::make_shared<std::function<void()>>();
      *next = [&ctx, stream, complete, buffer, total, next]
      {
        stream->async_read_some(net::buffer(*buffer), [&ctx, stream, complete, buffer, total, next]
          (beast::error_code ec, std::size_t n, bool done)
        {
          ASSERT_FALSE(ec) << ec.message(); *total += n;
          if (!done) return (*next)();
          ctx.set_body(std::to_string(*total)); complete();
        });
      };
      (*next)();
    });
  http::request<http::string_body> req{http::verb::post, "/chunked", 11};
  const std::string chunked_body = "chunked-body-with-several-pieces";
  req.body() = chunked_body;
  req.chunked(true);
  req.keep_alive(false);
  auto res = round_trip<http::string_body>(router, websocket_router, tree.web, std::move(req));
  EXPECT_EQ(res.result(), http::status::ok);
  EXPECT_EQ(res.body(), std::to_string(chunked_body.size()));
}

TEST(HttpSessionTest, ProxySessionStreamsUploadDownloadAndRangeHeaders)
{
  TempStaticTree tree;
  net::io_context upstream_ioc;
  tcp::acceptor upstream_acceptor(upstream_ioc, {net::ip::make_address("127.0.0.1"), 0});
  const auto upstream_endpoint = upstream_acceptor.local_endpoint();
  upstream_acceptor.async_accept([&](beast::error_code accept_ec, tcp::socket socket)
  {
    if (accept_ec) return;
    beast::flat_buffer buffer;
    http::request_parser<http::string_body> parser; parser.body_limit(8 * 1024 * 1024);
    beast::error_code ec; http::read(socket, buffer, parser, ec);
    if (ec) return;
    auto request = parser.release();
    http::response<http::string_body> response{http::status::partial_content, 11};
    response.set(http::field::content_range, "bytes 0-9/100");
    response.set(http::field::accept_ranges, "bytes");
    response.body() = std::move(request.body()); response.keep_alive(false); response.prepare_payload();
    http::write(socket, response, ec);
  });
  std::thread upstream_thread([&] { upstream_ioc.run(); });

  net::io_context proxy_ioc;
  auto proxy_guard = net::make_work_guard(proxy_ioc);
  std::thread proxy_thread([&] { proxy_ioc.run(); });
  khttpd_fw::HttpRouter router;
  khttpd_fw::WebsocketRouter websocket_router;
  std::shared_ptr<khttpd::framework::client::HttpProxySession> active_proxy;
  std::atomic<bool> proxy_handler_called{false};
  std::atomic<int> proxy_error{0};
  std::atomic<bool> proxy_completed{false};
  router.stream("/proxy", http::verb::post,
    [&](khttpd_fw::HttpContext& ctx, std::shared_ptr<khttpd_fw::HttpRequestStream> request_stream,
        std::shared_ptr<khttpd_fw::HttpResponseStream> response_stream, khttpd_fw::HttpStreamComplete)
    {
      proxy_handler_called = true;
      khttpd::framework::client::HttpClientStream::RequestHead head{ctx.method(), "/", ctx.get_request().version()};
      for (const auto& field : ctx.get_request()) head.insert(field.name_string(), field.value());
      active_proxy = std::make_shared<khttpd::framework::client::HttpProxySession>(
        proxy_ioc, std::move(request_stream), std::move(response_stream), 32 * 1024);
      const auto url = "http://127.0.0.1:" + std::to_string(upstream_endpoint.port()) + "/echo";
      active_proxy->start(url, std::move(head), [&](beast::error_code ec)
      { proxy_error = ec.value(); proxy_completed = true; });
    });

  const std::string payload(2 * 1024 * 1024 + 9, 'p');
  http::request<http::string_body> req{http::verb::post, "/proxy", 11};
  req.set(http::field::range, "bytes=0-9"); req.body() = payload; req.prepare_payload(); req.keep_alive(false);
  auto res = round_trip<http::string_body>(router, websocket_router, tree.web, std::move(req), false,
                                           [&] { return proxy_completed.load(); });

  proxy_guard.reset(); proxy_ioc.stop(); proxy_thread.join();
  beast::error_code ignored; upstream_acceptor.close(ignored); upstream_ioc.stop(); upstream_thread.join();
  EXPECT_TRUE(proxy_handler_called);
  EXPECT_TRUE(proxy_completed);
  EXPECT_EQ(proxy_error, 0);
  EXPECT_EQ(res.result(), http::status::partial_content);
  EXPECT_EQ(res[http::field::content_range], "bytes 0-9/100");
  EXPECT_EQ(res[http::field::accept_ranges], "bytes");
  EXPECT_EQ(res.body(), payload);
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

TEST(HttpSessionTest, WebSocketUpgradeRunsHttpAuthenticationFirst)
{
  struct DenyUpgrade final : khttpd_fw::Interceptor
  {
    khttpd_fw::InterceptorResult handle_request(khttpd_fw::HttpContext& ctx) override
    {
      ctx.set_status(http::status::unauthorized);
      ctx.set_body("websocket auth required");
      return khttpd_fw::InterceptorResult::Stop;
    }
  };
  TempStaticTree tree;
  net::io_context server_ioc;
  khttpd_fw::HttpRouter router;
  khttpd_fw::WebsocketRouter websocket_router;
  router.add_interceptor(std::make_shared<DenyUpgrade>());
  bool opened = false;
  websocket_router.add_handler("/private-ws", [&](khttpd_fw::WebsocketContext&) { opened = true; });
  tcp::acceptor acceptor(server_ioc, {net::ip::address_v4::loopback(), 0});
  const auto endpoint = acceptor.local_endpoint();
  acceptor.async_accept([&](beast::error_code ec, tcp::socket socket)
  {
    ASSERT_FALSE(ec);
    std::make_shared<khttpd_fw::HttpSession>(std::move(socket), router, websocket_router,
      tree.web.string(), fs::canonical(tree.web))->run();
  });
  std::thread server_thread([&] { server_ioc.run(); });
  net::io_context client_ioc;
  websocket::stream<tcp::socket> client(client_ioc);
  client.next_layer().connect(endpoint);
  websocket::response_type response;
  beast::error_code ec;
  client.handshake(response, "127.0.0.1", "/private-ws", ec);
  EXPECT_TRUE(ec);
  EXPECT_EQ(response.result(), http::status::unauthorized);
  EXPECT_FALSE(opened);
  beast::error_code ignored;
  client.next_layer().close(ignored);
  server_ioc.stop();
  server_thread.join();
}

TEST(HttpSessionTest, WebSocketHandshakePreservesRoutingAndRequestMetadata)
{
  TempStaticTree tree;
  net::io_context server_ioc;
  khttpd_fw::HttpRouter router;
  khttpd_fw::WebsocketRouter websocket_router;
  std::mutex captured_mutex;
  std::string target_param;
  khttpd_fw::WebsocketHandshakeRequest captured;
  std::atomic<bool> opened{false};
  std::atomic<bool> closed{false};

  websocket_router.add_handler("/gateway/:target", [&](khttpd_fw::WebsocketContext& ctx)
  {
    std::lock_guard<std::mutex> lock(captured_mutex);
    target_param = ctx.get_path_param("target").value_or("");
    captured = ctx.handshake();
    opened = true;
  }, nullptr, [&](khttpd_fw::WebsocketContext&) { closed = true; });

  tcp::acceptor acceptor(server_ioc, {net::ip::make_address("127.0.0.1"), 0});
  const auto endpoint = acceptor.local_endpoint();
  const auto canonical_web_root = fs::canonical(tree.web);
  acceptor.async_accept([&](beast::error_code ec, tcp::socket socket)
  {
    ASSERT_FALSE(ec) << ec.message();
    std::make_shared<khttpd_fw::HttpSession>(
      std::move(socket), router, websocket_router, tree.web.string(), canonical_web_root)->run();
  });
  std::thread server_thread([&] { server_ioc.run(); });

  net::io_context client_ioc;
  websocket::stream<tcp::socket> client(client_ioc);
  client.set_option(websocket::stream_base::decorator([](websocket::request_type& request)
  {
    request.set(http::field::authorization, "Bearer test-token");
    request.insert(http::field::cookie, "first=1");
    request.insert(http::field::cookie, "second=2");
    request.set(http::field::origin, "https://gateway-client.example");
    request.set(http::field::sec_websocket_protocol, "chat, telemetry");
  }));
  client.next_layer().connect(endpoint);
  client.handshake("127.0.0.1", "/gateway/orders/ws?tenant=acme&trace=abc");

  for (int i = 0; i < 100 && !opened; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  beast::error_code ignored;
  client.close(websocket::close_code::normal, ignored);
  for (int i = 0; i < 100 && !closed; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  server_ioc.stop();
  server_thread.join();

  ASSERT_TRUE(opened);
  ASSERT_TRUE(closed);
  std::lock_guard<std::mutex> lock(captured_mutex);
  EXPECT_EQ(target_param, "orders/ws");
  EXPECT_EQ(captured.target, "/gateway/orders/ws?tenant=acme&trace=abc");
  EXPECT_EQ(captured.path, "/gateway/orders/ws");
  EXPECT_EQ(captured.query_params.at("tenant"), "acme");
  EXPECT_EQ(captured.query_params.at("trace"), "abc");
  EXPECT_EQ(captured.subprotocols, (std::vector<std::string>{"chat", "telemetry"}));

  std::vector<std::string> cookies;
  for (const auto& [name, value] : captured.headers)
    if (name == "Cookie") cookies.push_back(value);
  EXPECT_EQ(cookies, (std::vector<std::string>{"first=1", "second=2"}));
}
