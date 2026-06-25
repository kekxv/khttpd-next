// app/main.cpp
#include <boost/filesystem.hpp>
#include <fstream>
#include "framework/server.hpp"
#include "framework/context/http_context.hpp"
#include "framework/context/websocket_context.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <thread>
#include <memory>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "HelloController.hpp"
#include "HelloStreamController.hpp"
#include "HelloWsController.hpp"

namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;
namespace beast = boost::beast;

int main(int argc, char* argv[])
{
  auto const address = net::ip::make_address("0.0.0.0");
  auto const port = static_cast<unsigned short>(8080);
  auto const num_threads = std::max<int>(1, static_cast<int>(std::thread::hardware_concurrency()));

  spdlog::info("Starting khttpd server with {} worker threads...", num_threads);

  // 定义 Web 根目录
  std::string web_root_path = "web_root";
  // 创建一个简单的 web_root 目录和文件用于测试
  boost::filesystem::create_directories(web_root_path);
  std::ofstream("web_root/index.html") <<
    "<html><body><h1>Hello from Static Index!</h1><p>Visit <a href=\"/hello_static.html\">hello_static.html</a></p></body></html>";
  std::ofstream("web_root/hello_static.html") <<
    R"(<html><body><h2>This is a static HTML file.</h2><img src="/image.png" alt="Static Image"/></body></html>)";

  auto server = std::make_shared<khttpd::framework::Server>(
    tcp::endpoint{address, port}, web_root_path, num_threads);

  auto& http_router = server->get_http_router();
  auto& ws_router = server->get_websocket_router();

  HelloController::create()->register_routes(http_router)->register_routes(ws_router);
  HelloStreamController::create()->register_routes(http_router)->register_routes(ws_router);
  HelloWsController::create()->register_routes(http_router)->register_routes(ws_router);

  http_router.get("/", [](khttpd::framework::HttpContext& ctx)
  {
    ctx.set_status(beast::http::status::ok);
    ctx.set_content_type("text/html");
    ctx.set_body(
      R"(<h1>Hello from khttpd!</h1><p>Try <a href="/hello?name=World">/hello?name=World</a> or <a href="/info">/info</a></p><p>Dynamic paths: <a href="/users/123">/users/123</a>, <a href="/users/profile">/users/profile</a>, <a href="/items/book/id/456">/items/book/id/456</a>, <a href="/files/a/b/c.txt">/files/a/b/c.txt</a></p><p>POST examples: /api/json, /api/form, /api/upload</p><p>Or connect to <a href="/ws">WebSocket</a></p><p>Or connect to <a href="/chat">WebSocket Chat</a></p>)");
  });

  http_router.get("/hello", [](khttpd::framework::HttpContext& ctx)
  {
    std::string name = ctx.get_query_param("name").value_or("Guest");
    ctx.set_status(beast::http::status::ok);
    ctx.set_content_type("text/plain");
    ctx.set_body(fmt::format("Hello, {}!", name));
  });

  http_router.get("/info", [](khttpd::framework::HttpContext& ctx)
  {
    std::string info_body = "<h1>Server Info</h1>";
    info_body += fmt::format("<p>Path: {}</p>", ctx.path());
    info_body += fmt::format("<p>Method: {}</p>", beast::http::to_string(ctx.method()));

    auto user_agent = ctx.get_header(beast::http::field::user_agent);
    if (user_agent)
    {
      info_body += fmt::format("<p>User-Agent: {}</p>", *user_agent);
    }
    else
    {
      info_body += "<p>User-Agent: Not provided</p>";
    }

    ctx.set_status(beast::http::status::ok);
    ctx.set_content_type("text/html");
    ctx.set_body(info_body);
  });

  http_router.post("/api/json", [](khttpd::framework::HttpContext& ctx)
  {
    if (auto json_value = ctx.get_json())
    {
      std::string response_body = "Received JSON:\n" + boost::json::serialize(*json_value);
      ctx.set_status(beast::http::status::ok);
      ctx.set_content_type("application/json");
      ctx.set_body(response_body);
    }
    else
    {
      ctx.set_status(beast::http::status::bad_request);
      ctx.set_content_type("text/plain");
      ctx.set_body("Invalid JSON or Content-Type not application/json");
    }
  });

  http_router.post("/api/form", [](khttpd::framework::HttpContext& ctx)
  {
    auto name = ctx.get_form_param("name");
    auto email = ctx.get_form_param("email");
    std::string response_body;

    if (name || email)
    {
      response_body = fmt::format("Received form data:\nName: {}\nEmail: {}",
                                  name.value_or("N/A"), email.value_or("N/A"));
      ctx.set_status(beast::http::status::ok);
      ctx.set_content_type("text/plain");
      ctx.set_body(response_body);
    }
    else
    {
      ctx.set_status(beast::http::status::bad_request);
      ctx.set_content_type("text/plain");
      ctx.set_body("Invalid form data or Content-Type not application/x-www-form-urlencoded");
    }
  });

  http_router.post("/api/upload", [](khttpd::framework::HttpContext& ctx)
  {
    auto content_type = ctx.get_header(beast::http::field::content_type);
    if (content_type && content_type->find("multipart/form-data") != std::string::npos)
    {
      std::string response_msg = "Multipart/form-data received!\n";

      if (auto description = ctx.get_multipart_field("description"))
      {
        response_msg += fmt::format("Description: {}\n", *description);
      }
      else
      {
        response_msg += "Description field not found.\n";
      }

      if (const auto* files = ctx.get_uploaded_files("file"))
      {
        if (!files->empty())
        {
          for (const auto& file : *files)
          {
            response_msg += fmt::format("Uploaded File: {} ({} bytes, type: {})\n", file.filename, file.data.length(),
                                        file.content_type);
            // spdlog::debug("File '{}' content: \n{}", file.filename, file.data);
          }
        }
        else
        {
          response_msg += "No files uploaded for field 'file'.\n";
        }
      }
      else
      {
        response_msg += "File field 'file' not found.\n";
      }

      ctx.set_status(beast::http::status::ok);
      ctx.set_content_type("text/plain");
      ctx.set_body(response_msg);
    }
    else
    {
      ctx.set_status(beast::http::status::bad_request);
      ctx.set_content_type("text/plain");
      ctx.set_body("Content-Type must be multipart/form-data for file uploads.");
    }
  });

  http_router.get("/users/profile", [](khttpd::framework::HttpContext& ctx)
  {
    ctx.set_status(beast::http::status::ok);
    ctx.set_content_type("text/plain");
    ctx.set_body("This is the user's profile page.");
  });

  http_router.get("/users/:id", [](khttpd::framework::HttpContext& ctx)
  {
    auto user_id = ctx.get_path_param("id").value_or("unknown");
    ctx.set_status(beast::http::status::ok);
    ctx.set_content_type("text/plain");
    ctx.set_body(fmt::format("Fetching user with ID: {}", user_id));
  });

  http_router.get("/items/:category/id/:item_id", [](khttpd::framework::HttpContext& ctx)
  {
    auto category = ctx.get_path_param("category").value_or("N/A");
    auto item_id = ctx.get_path_param("item_id").value_or("N/A");
    ctx.set_status(beast::http::status::ok);
    ctx.set_content_type("text/plain");
    ctx.set_body(fmt::format("Fetching item {} in category {}", item_id, category));
  });

  http_router.get("/files/:filepath", [](khttpd::framework::HttpContext& ctx)
  {
    auto filepath = ctx.get_path_param("filepath").value_or("N/A");
    ctx.set_status(beast::http::status::ok);
    ctx.set_content_type("text/plain");
    ctx.set_body(fmt::format("Accessing file: {}", filepath));
  });

  http_router.get("/ws", [](khttpd::framework::HttpContext& ctx)
  {
    ctx.set_status(beast::http::status::upgrade_required);
    ctx.set_header(beast::http::field::upgrade, "websocket");
    ctx.set_content_type("text/html");
    ctx.set_body(
      "<h1>WebSocket Echo Endpoint</h1><p>This is a WebSocket endpoint. Please use a WebSocket client to connect.</p>");
  });

  http_router.get("/chat", [](khttpd::framework::HttpContext& ctx)
  {
    ctx.set_status(beast::http::status::upgrade_required);
    ctx.set_header(beast::http::field::upgrade, "websocket");
    ctx.set_content_type("text/html");
    ctx.set_body(
      "<h1>WebSocket Chat Endpoint</h1><p>This is a WebSocket chat endpoint. Please use a WebSocket client to connect.</p>");
  });

  ws_router.add_handler(
    "/ws",
    // onopen
    []([[maybe_unused]] khttpd::framework::WebsocketContext& ctx)
    {
      spdlog::info("[WS: {}] Connection opened.", ctx.path);
      ctx.send("Welcome to the echo service!");
    },
    // onmessage
    [](khttpd::framework::WebsocketContext& ctx)
    {
      spdlog::info("[WS: {}] Received: {}", ctx.path, ctx.message);
      ctx.send("Echo: " + ctx.message, ctx.is_text);
    },
    // onclose
    []([[maybe_unused]] khttpd::framework::WebsocketContext& ctx)
    {
      spdlog::info("[WS: {}] Connection closed.", ctx.path);
    },
    // onerror
    [](khttpd::framework::WebsocketContext& ctx)
    {
      spdlog::error("[WS: {}] Error: {}", ctx.path, ctx.error_code.message());
    }
  );

  ws_router.add_handler(
    "/chat",
    // onopen
    []([[maybe_unused]] khttpd::framework::WebsocketContext& ctx)
    {
      spdlog::info("[WS: {}] Chat connection opened. Say hello!", ctx.path);
      ctx.send("Welcome to the chat!");
    },
    // onmessage
    [](khttpd::framework::WebsocketContext& ctx)
    {
      spdlog::info("[WS: {}] Chat message: {}", ctx.path, ctx.message);
      ctx.send("You said: " + ctx.message, ctx.is_text);
    },
    // onclose
    []([[maybe_unused]] khttpd::framework::WebsocketContext& ctx)
    {
      spdlog::info("[WS: {}] Chat connection closed.", ctx.path);
    },
    // onerror
    [](khttpd::framework::WebsocketContext& ctx)
    {
      spdlog::error("[WS: {}] Chat error: {}", ctx.path, ctx.error_code.message());
    }
  );

  server->run();

  spdlog::info("Application exited.");

  return 0;
}
