# khttpd

A high-performance, header-only-style HTTP/WebSocket server framework built on top
of [Boost.Beast](https://www.boost.org/doc/libs/release/libs/beast/)
and [Boost.Asio](https://www.boost.org/doc/libs/release/libs/asio/), managed with [Bazel](https://bazel.build/).

[文档](doc/index.md)

## Features

- **HTTP Server** — Multi-threaded, async I/O server powered by Boost.Asio strand-based concurrency
- **WebSocket Support** — Dynamic routes, handshake metadata, typed text/binary/control frames, and full lifecycle management
- **Routing** — Express-style route registration with path parameters (`/users/:id` or `/users/{id}`), query params,
  and method specificity sorting
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
- **Async Server-Sent Events** — Non-blocking SSE routes and client with incremental parsing, cancellation, and backpressure
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

### Custom error pages

The router renders responsive HTML pages for 404 and 405 responses by default. A service can replace either response
without reimplementing route matching. The framework sets the status before calling the handler, and it sets `Allow`
before calling a 405 handler:

```cpp
router.set_not_found_handler([](khttpd::framework::HttpContext& ctx) {
    ctx.set_content_type("application/json");
    ctx.set_body(R"({"error":"not_found"})"); // Status is already 404.
});

router.set_method_not_allowed_handler([](khttpd::framework::HttpContext& ctx) {
    ctx.set_content_type("application/json");
    ctx.set_body(R"({"error":"method_not_allowed"})"); // Status is 405; Allow is preserved.
});
```

Pass an empty `khttpd::framework::HttpHandler{}` to either setter to restore the framework default. Custom handlers can inspect the request
path and headers, so an auth service can return branded HTML to browsers and a JSON error envelope to API clients.

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
│   ├── sse_client.hpp/cpp      # Async Server-Sent Events client
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
├── sse/
│   ├── sse_parser.hpp/cpp      # Incremental SSE parser and wire formatting
│   └── sse_session.hpp/cpp     # Async server-side SSE write queue
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

Invalid media types, malformed JSON, and DTO conversion failures throw `TypedRequestValidationError` through the router's
exception pipeline. Without a mapper they retain the default `400 INVALID_REQUEST_BODY` response; applications that use a
shared error envelope can map them once for every typed route:

```cpp
router.map_exception<khttpd::framework::TypedRequestValidationError>(
    [](const auto& error) {
        return khttpd::framework::HttpResult<ErrorResponse>(
            boost::beast::http::status::bad_request,
            {"INVALID_REQUEST", error.what()});
    });
```

### OpenAPI 3.1 documentation

Route registration also records handler-free documentation metadata. Legacy routes contribute their method, path, and path
parameters; typed routes additionally contribute request and response schemas. DTOs declared with `BOOST_DESCRIBE_STRUCT`
produce field-level schemas, while DTOs that only provide custom Boost.JSON converters use a conservative `object` schema.

Add field descriptions after `BOOST_DESCRIBE_STRUCT` with the OpenAPI field documentation macros. They only enrich
`openapi.json` and `/docs`; Boost.JSON conversion and the DTO remain unchanged:

```cpp
KHTTPD_OPENAPI_FIELD_DOCUMENTATION(CreateUserRequest,
  KHTTPD_OPENAPI_FIELD(name, "Name of the new user.")
  KHTTPD_OPENAPI_FIELD(age, "Age of the new user."))
```

Add a summary and a longer description when registering a route; both become standard OpenAPI operation fields and appear in
`/docs`:

```cpp
router.post("/messages", handle_message,
            {"Send a message", "Accepts a message and returns its delivery result."});
```

Document request headers with a name, description, and required flag. They are emitted as OpenAPI `in: header`
parameters and appear in the interactive `/docs` request form:

```cpp
router.post("/tokens", create_token,
            {"Create token", "Creates an access token.",
             {{"Authorization", "Bearer access token.", true},
              {"X-Request-Id", "Optional caller correlation identifier.", false}}});
```

Header metadata documents the API only; it does not authenticate or validate incoming requests. Read and validate the
header in the handler (for example, with `HttpContext::get_header`) as part of the service's normal authorization flow.

For legacy `HttpContext` handlers, provide explicit JSON Schema values as the fourth and fifth
`RouteDocumentation` fields. They describe the request body and successful response without changing runtime parsing:

```cpp
router.post("/authorize", authorize,
            {"Authorize", "Checks an API token.", {},
             {{"type", "object"}, {"properties", {{"token", {{"type", "string"}}}}},
              {"required", {"token"}}},
             {{"type", "object"}, {"properties", {{"authorized", {{"type", "boolean"}}}}}}});
```

For an already registered async or stream route, call
`router.document_route("/messages", boost::beast::http::verb::post, {"Send a message", "..."})` afterwards. Controllers
can use `KHTTPD_DOCUMENTED_ROUTE` or `KHTTPD_DOCUMENTED_TYPED_ROUTE` with the same `RouteDocumentation` value.

```cpp
#include "framework/router/openapi.hpp"

// Register application routes first, then add hidden runtime documentation routes.
khttpd::framework::install_openapi_routes(
    server->get_http_router(), {"Example API", "1.0.0"});
// GET /openapi.json and GET /docs
```

Both `/docs` and `/openapi.json` are implemented by the framework; the example only demonstrates calling
`install_openapi_routes`. `/docs` is a responsive, dependency-free HTML view with endpoint navigation, parsed schema
fields, generated request examples, cURL copy, and an interactive request runner. Interactive requests omit browser
credentials by default. The exported
`openapi.json` is an OpenAPI document, not a single JSON Schema: do not pass the whole file to an OpenAI
`response_format.json_schema` field. Select the relevant request/response `schema` below `paths`, or convert that operation
schema for the target consumer. Unreflected C++ object types are emitted conservatively as
`{"type":"object","properties":{}}`.

The example includes documented legacy lambda routes (`/`, `/hello`, `/api/json`), documented controller routes
(`/stream/:size` and `/hello/hello`), and documented typed `POST /typed/greetings`, along with `HttpResult<T>`
headers/status and a serialized validation exception. Run it normally on port 8080, or export the exact same registered
routes and exit without constructing a listening server:

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

### Async Server-Sent Events

Register an SSE endpoint with `HttpRouter::sse`. The session serializes writes
through an owned queue, so callers may publish from different threads without
keeping event strings alive until the socket write completes.

```cpp
router.sse("/events",
  [](HttpContext&, std::shared_ptr<sse::SseSession> session)
  {
    session->on_close([](boost::system::error_code ec) {
      // Remove the subscriber from application state.
    });
    session->send({"config", R"({"name":"app.yaml"})", "42", 3000});
    session->send_comment("heartbeat");

    // Retain session in the application's subscriber collection when more
    // events will be sent after this handler returns. Call close() for a
    // graceful final chunk or cancel() to abort the connection.
  });
```

The optional third `router.sse` argument sets the maximum queued wire bytes per
connection (1 MiB by default). `send()` returns `false` when that limit would be
exceeded, allowing the application to drop or resynchronize a slow subscriber.
SSE routes run the ordinary pre-request interceptor chain before their handler;
an authentication or authorization interceptor can reject the request before
any event-stream response headers are written.

The server also monitors the connection's read side while an SSE response is
open. A passive client disconnect completes the session and invokes `on_close`
even when the application has no event or heartbeat waiting to be written.

`SseClient` uses the same fixed-buffer streaming transport and delivers each
complete event as soon as it arrives. Arbitrarily split lines, CRLF/LF,
multiline `data`, comments, `id`, and numeric `retry` fields are supported.

The starter `example` exposes a visual `/events-demo` page and the raw `/events`
stream. After `bazel run //:app`, open the page in a browser or inspect the stream
with `curl -N http://127.0.0.1:8080/events`.

```cpp
auto events = std::make_shared<client::SseClient>(ioc); // 1 MiB event limit
events->connect(
  "https://config.internal/events",
  {{"Authorization", "Bearer internal-token"}, {"Last-Event-ID", "41"}},
  [](const sse::SseEvent& event) {
    // event.event, event.data, event.id, event.retry
  },
  [](boost::system::error_code ec) {
    // Schedule reconnect/backoff in application code when appropriate.
  });
```

Keep the `SseClient` alive for the duration of the subscription. `cancel()`
closes the transport and completes the close callback once with
`operation_aborted`. Automatic reconnect is intentionally left to the caller,
which can apply service-specific backoff and send `Last-Event-ID`.
The optional constructor limit bounds an unfinished line or event; exceeding it
closes only that subscription and reports `message_size`.

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
