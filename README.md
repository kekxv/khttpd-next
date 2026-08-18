# khttpd

A high-performance, header-only-style HTTP/WebSocket server framework built on top
of [Boost.Beast](https://www.boost.org/doc/libs/release/libs/beast/)
and [Boost.Asio](https://www.boost.org/doc/libs/release/libs/asio/), managed with [Bazel](https://bazel.build/).

[文档](doc/index.md)

## Features

- **HTTP Server** — Multi-threaded, async I/O server powered by Boost.Asio strand-based concurrency
- **WebSocket Support** — Dynamic routes, handshake metadata, typed text/binary/control frames, and full lifecycle management
- **Routing** — Express-style route registration with path parameters (`/users/:id`), query params, and method
  specificity sorting
- **Controller Pattern** — CRTP-based `BaseController` with `KHTTPD_ROUTE` / `KHTTPD_WSROUTE` macros for clean route
  definitions
- **Typed JSON Routes** — Request/response inference for lambdas and controller members, `HttpResult<T>` status/headers,
  and serializable exception mapping while retaining the original `HttpContext&` API
- **HTTP Client** — Sync & async HTTP client with SSL, bearer token, base URL, and JSON body serialization
- **Oat++-style API Client** — Declarative API definition with `KHTTPD_API_CLIENT`, multi-host support with weight-based routing
- **WebSocket Client** — Async WebSocket client counterpart
- **Interceptors** — Pre-request / post-response middleware pipeline
- **Exception Handling** — Type-safe exception dispatcher with per-type handlers
- **Chunked Streaming** — Server-sent chunked transfer encoding via `HttpContext::chunked()`
- **Bidirectional HTTP Streaming** — Header-first request routing, fixed-buffer upload/download, proxy backpressure, and Range forwarding
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
| Boost               | 1.90.0         |
| Boost.Beast         | 1.90.0         |
| Boost.Asio          | 1.90.0         |
| fmt                 | 12.1.0         |
| OpenSSL / BoringSSL | 4.0.1 / 0.20260616.0 |
| SQLite3             | 3.53.2         |
| Build System        | Bazel (bzlmod) |

## Quick Start

### 1. Add khttpd as a Bazel dependency

In your project's `MODULE.bazel`:

```python
http_archive = use_repo_rule("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

bazel_dep(name="platforms", version="1.1.0")
bazel_dep(name="rules_cc", version="0.2.20")
bazel_dep(name="fmt", version="12.1.0")
bazel_dep(name="boost", version="1.90.0.bcr.1")
bazel_dep(name="boost.asio", version="1.90.0.bcr.1")
bazel_dep(name="boost.beast", version="1.90.0.bcr.1")
bazel_dep(name="boost.json", version="1.90.0.bcr.1")
bazel_dep(name="boost.filesystem", version="1.90.0.bcr.1")
bazel_dep(name="boost.url", version="1.90.0.bcr.1")
bazel_dep(name="boost.uuid", version="1.90.0.bcr.1")
bazel_dep(name="boringssl", version="0.20260616.0")
bazel_dep(name="spdlog", version="1.17.0")

http_archive(
  name="khttpd",
  strip_prefix="khttpd-0.3.0",
  url="https://github.com/ClangTools/khttpd/archive/refs/tags/v0.3.0.tar.gz",
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

    // Buffered JSON/form routes default to 16 MiB. Configure the limit in bytes.
    server->set_max_buffered_request_body_size(32ULL * 1024 * 1024);

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
│   ├── http_request_stream.hpp # Fixed-buffer inbound request body
│   ├── http_response_stream.hpp # Fixed-buffer outbound response body
│   └── websocket_context.hpp/cpp # WebSocket session context (send, attributes)
├── router/
│   ├── http_router.hpp/cpp     # Route matching, interceptors, exception dispatch
│   └── websocket_router.hpp/cpp # WS lifecycle handler registration
├── controller/
│   └── http_controller.hpp     # CRTP BaseController + KHTTPD_ROUTE / KHTTPD_WSROUTE macros
├── client/
│   ├── http_client.hpp/cpp     # Sync/async HTTP client with SSL
│   ├── http_client_stream.hpp/cpp # Fixed-buffer HTTP streaming client
│   ├── http_proxy_session.hpp/cpp # Bidirectional streaming proxy pump
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
ws.add_handler("/gateway/:target",
    [](WebsocketContext& ctx) {
        auto target = ctx.get_path_param("target"); // may contain multiple path segments
        auto token = ctx.get_header("Authorization");
        auto trace = ctx.get_query_param("trace");
        ctx.send("Welcome!");
    },
    [](WebsocketContext& ctx) {
        // frame.type preserves text/binary; payload may contain arbitrary bytes.
        ctx.send(ctx.frame);
    },
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

### Typed JSON routes

Typed routes infer the request and response from a callable or controller member. DTOs described with Boost.Describe work
with the framework's Boost.JSON conversion support:

```cpp
struct CreateUserRequest { std::string name; int age; };
struct UserResponse { int id; std::string name; };

BOOST_DESCRIBE_STRUCT(CreateUserRequest, (), (name, age))
BOOST_DESCRIBE_STRUCT(UserResponse, (), (id, name))

class UserController final : public khttpd::framework::BaseController<UserController> {
public:
    std::shared_ptr<BaseController> register_routes(HttpRouter& router) override {
        KHTTPD_TYPED_ROUTE(post, "/users", create_user);
        return shared_from_this();
    }

private:
    khttpd::framework::HttpResult<UserResponse> create_user(const CreateUserRequest& request) {
        auto result = khttpd::framework::HttpResult<UserResponse>::created({1001, request.name});
        return result.header("Location", "/users/1001");
    }
};
```

A bare JSON-serializable response is automatically returned as JSON with status 200. Return `HttpResult<T>` when status or
headers are required, and `HttpResult<void>` for an empty response. The optional second handler argument may be
`HttpContext&` for headers, cookies, path parameters, and interceptor attributes.

Typed request bodies require `application/json` or an `application/*+json` media type. Invalid bodies receive a stable 400
response without invoking the handler. Typed routes register as ordinary buffered routes, so interceptors and authorization
checks run before them exactly as they do for legacy routes.

### OpenAPI 3.1 documentation

Route registration also records handler-free documentation metadata. Legacy routes contribute their method, path, and path
parameters; typed routes additionally contribute request and response schemas. DTOs declared with `BOOST_DESCRIBE_STRUCT`
produce field-level schemas, while DTOs that only provide custom Boost.JSON converters use a conservative `object` schema.

```cpp
#include "framework/router/openapi.hpp"

// Register application routes first, then add hidden runtime documentation routes.
khttpd::framework::install_openapi_routes(
    server->get_http_router(), {"Example API", "1.0.0"});
// GET /openapi.json and GET /docs
```

The example includes `POST /typed/greetings`, `HttpResult<T>` headers/status, and a serialized validation exception. Run it
normally on port 8080, or export the exact same registered routes and exit without constructing a listening server:

```bash
bazel run //example:app
bazel run //example:app -- --export-openapi openapi.json
bazel run //example:app -- --disable-openapi-docs
```

`install_openapi_routes` rejects dynamic, control-character, duplicate, or conflicting documentation paths. Documentation
routes are omitted from their own document and use the ordinary router/interceptor pipeline. `export_openapi` writes only to
the caller-supplied path; applications should apply their normal filesystem authorization policy before accepting such a path
from an untrusted user. Runtime routes can be switched explicitly with the final `enabled` argument; the example accepts
`--enable-openapi-docs` and `--disable-openapi-docs` (the last flag wins).

### Streaming HTTP routes and proxying

Large request and response bodies can bypass `string_body` buffering by using a
stream route. Reads and writes are serialized through fixed-size buffers, so
backpressure is propagated between the downstream and upstream connections.

```cpp
router.stream("/gateway/upload", boost::beast::http::verb::post,
  [](HttpContext& ctx,
     std::shared_ptr<HttpRequestStream> request,
     std::shared_ptr<HttpResponseStream> response,
     HttpStreamComplete complete)
  {
    client::HttpClientStream::RequestHead head{
      ctx.method(), "/upload", ctx.get_request().version()};
    for (const auto& field : ctx.get_request())
      head.insert(field.name_string(), field.value());

    auto proxy = std::make_shared<client::HttpProxySession>(request, response);
    proxy->start("http://upstream.internal/upload", std::move(head));
  });
```

Normal routes remain buffered for JSON/form compatibility and reject request
bodies above the configurable limit (16 MiB by default; use
`Server::set_max_buffered_request_body_size(bytes)`). Stream routes are not
subject to this buffered-body limit and have no size-dependent allocation;
the proxy buffer defaults to 64 KiB and can be configured in its constructor.

`HttpClientStream` supports both `http://` and `https://` without changing its
fixed-buffer behavior. Its default TLS context verifies the system trust store;
an application can inject an `ssl::context` into `HttpClientStream` or
`HttpProxySession` for private CAs and test certificates.

### Tests

Run the complete framework suite, including buffered-body boundaries, streaming
edge cases, proxy cancellation, WebSocket dynamic routing, and frame fidelity:

```bash
bazel test //framework/... --test_output=errors
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

### Oat++-style API Client

```cpp
#include "framework/client/api_macros.hpp"

// 单 host
KHTTPD_API_CLIENT(GitHubClient, "https://api.github.com")
    API_CALL(http::verb::get, "/users/:login", get_user,
             PATH(std::string, login, "login"))
KHTTPD_API_CLIENT_END()

// 多 host + 权重分发
KHTTPD_API_CLIENT_POOL(GitHubClient,
    KHTTPD_HOST("https://api.github.com", 3)
    KHTTPD_HOST("https://api-backup.github.com", 1)
)
    API_CALL(http::verb::get, "/users/:login", get_user,
             PATH(std::string, login, "login"))
KHTTPD_API_CLIENT_END()

// 使用
auto gh = std::make_shared<GitHubClient>();
auto res = gh->get_user_sync("octocat");      // 同步
gh->get_user("octocat", [](auto ec, auto res) { /* 异步 */ });
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
#include "framework/exception/http_exception.hpp"

router.map_exception<ValidationError>([](const ValidationError& e) {
    return khttpd::framework::HttpResult<ErrorResponse>(
        boost::beast::http::status::unprocessable_entity,
        {"VALIDATION_FAILED", e.what()});
});
```

`HttpException` is available when an exception should carry an HTTP status, JSON body, and validated headers directly.
Unmapped exceptions return a generic JSON 500 response; exception details are logged server-side but are not sent to clients.

## License

MIT License — see [LICENSE](LICENSE) for details.
