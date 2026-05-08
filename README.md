# khttpd

A high-performance, header-only-style HTTP/WebSocket server framework built on top
of [Boost.Beast](https://www.boost.org/doc/libs/release/libs/beast/)
and [Boost.Asio](https://www.boost.org/doc/libs/release/libs/asio/), managed with [Bazel](https://bazel.build/).

[文档](doc/index.md)

## Features

- **HTTP Server** — Multi-threaded, async I/O server powered by Boost.Asio strand-based concurrency
- **WebSocket Support** — Full WebSocket lifecycle management (onopen / onmessage / onclose / onerror)
- **Routing** — Express-style route registration with path parameters (`/users/:id`), query params, and method
  specificity sorting
- **Controller Pattern** — CRTP-based `BaseController` with `KHTTPD_ROUTE` / `KHTTPD_WSROUTE` macros for clean route
  definitions
- **HTTP Client** — Sync & async HTTP client with SSL, bearer token, base URL, and JSON body serialization
- **WebSocket Client** — Async WebSocket client counterpart
- **Interceptors** — Pre-request / post-response middleware pipeline
- **Exception Handling** — Type-safe exception dispatcher with per-type handlers
- **Chunked Streaming** — Server-sent chunked transfer encoding via `HttpContext::chunked()`
- **Cookie Support** — Read / write cookies with configurable `CookieOptions` (path, domain, SameSite, etc.)
- **Form & Multipart** — `application/x-www-form-urlencoded` and `multipart/form-data` parsing (file uploads)
- **JSON** — Native `boost::json` integration with `get_json()`, `set_body_json()`, `set_body_from()`
- **Cron Scheduler** — Singleton-based cron task scheduler with cron expressions
- **Dependency Injection** — Type-indexed singleton DI container with constructor dependency resolution
- **Static Files** — Built-in static file serving with configurable web root
- **Signal Handling** — Graceful shutdown on SIGINT / SIGTERM

## Tech Stack

| Component           | Version        |
|---------------------|----------------|
| Boost               | 1.89.0         |
| Boost.Beast         | 1.89.0         |
| Boost.Asio          | 1.89.0         |
| fmt                 | 12.0.0         |
| OpenSSL / BoringSSL | 3.3.1 / latest |
| SQLite3             | 3.50.4         |
| Build System        | Bazel (bzlmod) |

## Quick Start

### 1. Add khttpd as a Bazel dependency

In your project's `MODULE.bazel`:

```python
http_archive = use_repo_rule("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

bazel_dep(name="platforms", version="1.0.0")
bazel_dep(name="rules_cc", version="0.2.13")
bazel_dep(name="fmt", version="12.0.0")
bazel_dep(name="boost", version="1.89.0.bcr.2")
bazel_dep(name="boost.asio", version="1.89.0.bcr.2")
bazel_dep(name="boost.beast", version="1.89.0.bcr.2")
bazel_dep(name="boost.json", version="1.89.0.bcr.2")
bazel_dep(name="boost.filesystem", version="1.89.0.bcr.2")
bazel_dep(name="boost.url", version="1.89.0.bcr.2")
bazel_dep(name="boringssl", version="0.20251110.0")

http_archive(
  name="khttpd",
  strip_prefix="khttpd-0.1.0",
  url="https://github.com/ClangTools/khttpd/archive/refs/tags/v0.1.0.tar.gz",
)
```

### 2. Create your server

```cpp
#include "framework/server.hpp"
#include "framework/context/http_context.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <fmt/format.h>

namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

int main() {
    auto server = std::make_shared<khttpd::framework::Server>(
        tcp::endpoint{net::ip::make_address("0.0.0.0"), 8080},
        "web_root",                          // static file root
        std::thread::hardware_concurrency()  // worker threads
    );

    auto& router = server->get_http_router();

    // Simple route
    router.get("/hello", [](khttpd::framework::HttpContext& ctx) {
        std::string name = ctx.get_query_param("name").value_or("World");
        ctx.set_status(boost::beast::http::status::ok);
        ctx.set_content_type("text/plain");
        ctx.set_body(fmt::format("Hello, {}!", name));
    });

    // JSON endpoint
    router.post("/api/data", [](khttpd::framework::HttpContext& ctx) {
        if (auto json = ctx.get_json()) {
            ctx.set_body_from(*json);
        }
    });

    // Path parameters
    router.get("/users/:id", [](khttpd::framework::HttpContext& ctx) {
        auto id = ctx.get_path_param("id").value_or("unknown");
        ctx.set_body(fmt::format("User: {}", id));
    });

    server->run();
    return 0;
}
```

### 3. Build and run

```bash
bazel build //:your_target
bazel run //:your_target
```

## Architecture

```
framework/
├── server.hpp/cpp              # Main server: acceptor, signal handling, thread pool
├── io_context_pool.hpp         # Asio io_context thread pool
├── context/
│   ├── http_context.hpp/cpp    # Request/response abstraction (params, body, cookies, streaming)
│   └── websocket_context.hpp/cpp # WebSocket session context (send, attributes)
├── router/
│   ├── http_router.hpp/cpp     # Route matching, interceptors, exception dispatch
│   └── websocket_router.hpp/cpp # WS lifecycle handler registration
├── controller/
│   └── http_controller.hpp     # CRTP BaseController + KHTTPD_ROUTE / KHTTPD_WSROUTE macros
├── client/
│   ├── http_client.hpp/cpp     # Sync/async HTTP client with SSL
│   └── websocket_client.hpp/cpp # WebSocket client
├── interceptor/
│   └── interceptor.hpp         # Pre/Post middleware interface
├── exception/
│   └── exception_handler.hpp   # Type-safe exception dispatcher
├── cron/
│   ├── CronJob.hpp             # Cron job base class
│   ├── CronScheduler.hpp       # Singleton scheduler
│   └── cronacci.hpp            # Cron expression parser
├── di/
│   └── di_container.hpp        # Type-indexed DI container (singleton)
├── session/
│   └── http_session.hpp/cpp    # Per-connection HTTP session
└── websocket/
    └── websocket_session.hpp/cpp # Per-connection WebSocket session
```

## API Reference

### HttpContext

| Method                         | Description                                      |
|--------------------------------|--------------------------------------------------|
| `path()`                       | Request path                                     |
| `method()`                     | HTTP verb                                        |
| `get_query_param(key)`         | Query string parameter                           |
| `get_path_param(key)`          | Path parameter (from `:param` routes)            |
| `get_header(name)`             | Request header                                   |
| `get_cookie(key)`              | Cookie value                                     |
| `get_json()`                   | Parse body as `boost::json::value`               |
| `get_form_param(key)`          | Form field (`application/x-www-form-urlencoded`) |
| `get_multipart_field(key)`     | Multipart text field                             |
| `get_uploaded_files(field)`    | Uploaded files as `vector<MultipartFile>`        |
| `set_status(code)`             | Response status                                  |
| `set_body(str)`                | Response body                                    |
| `set_body_json(obj)`           | Serialize object to JSON response                |
| `set_body_from(obj)`           | `value_from` + JSON response                     |
| `set_content_type(type)`       | Content-Type header                              |
| `set_header(name, value)`      | Custom response header                           |
| `set_cookie(key, value, opts)` | Set response cookie                              |
| `chunked(handler)`             | Enable chunked transfer streaming                |
| `set_attribute(key, value)`    | Store arbitrary data (for interceptors)          |
| `get_attribute_as<T>(key)`     | Retrieve typed attribute                         |

### WebSocket

```cpp
auto& ws = server->get_websocket_router();
ws.add_handler("/ws",
    [](WebsocketContext& ctx) { /* onopen  */ ctx.send("Welcome!"); },
    [](WebsocketContext& ctx) { /* onmessage */ ctx.send("Echo: " + ctx.message, ctx.is_text); },
    [](WebsocketContext& ctx) { /* onclose  */ },
    [](WebsocketContext& ctx) { /* onerror  */ }
);
```

### Controller Pattern

```cpp
class MyController : public khttpd::framework::BaseController<MyController> {
    std::string base_path() override { return "/api"; }

    std::shared_ptr<BaseController> register_routes(HttpRouter& router) override {
        KHTTPD_ROUTE(get, "/items", handle_list);
        KHTTPD_ROUTE(get, "/items/:id", handle_get);
        return shared_from_this();
    }

    void handle_list(HttpContext& ctx) { /* ... */ }
    void handle_get(HttpContext& ctx) { /* ... */ }
};

// Register
MyController::create()->register_routes(server->get_http_router());
```

### Interceptors

```cpp
struct AuthInterceptor : khttpd::framework::Interceptor {
    InterceptorResult handle_request(HttpContext& ctx) override {
        if (!ctx.get_header("Authorization")) {
            ctx.set_status(boost::beast::http::status::unauthorized);
            ctx.set_body("Unauthorized");
            return InterceptorResult::Stop;
        }
        return InterceptorResult::Continue;
    }
};

server->add_interceptor(std::make_shared<AuthInterceptor>());
```

### Cron Scheduler

```cpp
#include "framework/cron/CronScheduler.hpp"

auto& scheduler = khttpd::framework::CronScheduler::instance();
scheduler.schedule("0 */5 * * * *", []() {  // every 5 minutes
    fmt::print("Cron tick!\n");
});
```

### HTTP Client

```cpp
#include "framework/client/http_client.hpp"

auto client = std::make_shared<khttpd::framework::client::HttpClient>();
client->set_base_url("https://api.example.com");
client->set_bearer_token("your-token");

// Async
client->request(http::verb::get, "/users", {}, {}, {},
    [](beast::error_code ec, http::response<http::string_body> res) {
        if (!ec) fmt::print("{}\n", res.body());
    });

// Sync
auto res = client->request_sync(http::verb::post, "/data", {}, "{\"key\":\"val\"}", {});
```

### Dependency Injection

```cpp
#include "framework/di/di_container.hpp"

auto& di = khttpd::framework::DI_Container::instance();
di.register_component<DatabaseService>();
di.register_component<UserRepository, DatabaseService>();

auto repo = di.resolve<UserRepository>();
```

### Exception Handling

```cpp
#include "framework/exception/exception_handler.hpp"

auto dispatcher = std::make_shared<khttpd::framework::ExceptionDispatcher>();
dispatcher->on<std::runtime_error>([](const std::runtime_error& e, HttpContext& ctx) {
    ctx.set_status(boost::beast::http::status::internal_server_error);
    ctx.set_body(fmt::format("Error: {}", e.what()));
});
server->get_http_router().add_exception_handler(dispatcher);
```

## License

MIT License — see [LICENSE](LICENSE) for details.
