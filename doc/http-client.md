# HTTP 与 WebSocket 客户端

khttpd 内置 HTTP 和 WebSocket 客户端，方便在同一个项目中同时提供服务端和客户端能力。

## HTTP 客户端

### 基本使用

```cpp
#include "framework/client/http_client.hpp"

namespace net = boost::asio;

// 方式 1: 使用全局 IO 池（推荐，最简单）
auto client = std::make_shared<khttpd::framework::client::HttpClient>();

// 方式 2: 指定 IO 上下文
net::io_context ioc;
auto client = std::make_shared<khttpd::framework::client::HttpClient>(ioc);
```

### 配置

```cpp
client->set_base_url("https://api.example.com");
client->set_bearer_token("your-jwt-token");
client->set_default_header("X-App-Version", "1.0.0");
client->set_timeout(std::chrono::seconds(30));
```

### 同步请求

```cpp
try {
    auto res = client->request_sync(
        http::verb::get,
        "/users",
        {{"page", "1"}, {"limit", "20"}},  // 查询参数
        "",                                 // 请求体
        {}                                  // 额外请求头
    );

    fmt::print("Status: {}\n", res.result());
    fmt::print("Body: {}\n", res.body());
} catch (const std::exception& e) {
    fmt::print(stderr, "Request failed: {}\n", e.what());
}
```

### 异步请求

```cpp
client->request(
    http::verb::post,
    "/users",
    {},
    R"({"name":"Alice","email":"alice@example.com"})",
    {{"Content-Type", "application/json"}},
    [](beast::error_code ec, http::response<http::string_body> res) {
        if (!ec) {
            fmt::print("Response: {}\n", res.body());
        } else {
            fmt::print(stderr, "Error: {}\n", ec.message());
        }
    }
);
```

### API_CALL 宏（自动生成客户端方法）

在类中定义 API 方法，自动生成异步和同步版本：

```cpp
class GitHubClient : public HttpClient {
public:
    GitHubClient() {
        set_base_url("https://api.github.com");
    }

    // 生成: get_user(username, callback) 和 get_user_sync(username)
    API_CALL(http::verb::get, "/users/:login", get_user,
             PATH(std::string, login, "login"))

    // 生成: list_repos(username, page, per_page, callback) 和同步版本
    API_CALL(http::verb::get, "/users/:login/repos", list_repos,
             PATH(std::string, login, "login"),
             QUERY(int, page, "page"),
             QUERY(int, per_page, "per_page"))

    // 生成: create_repo(body, callback) 和同步版本
    API_CALL(http::verb::post, "/user/repos", create_repo,
             BODY(boost::json::object, repo_data))

    // 生成: get_repo(login, repo_name, callback)
    API_CALL(http::verb::get, "/repos/:login/:repo", get_repo,
             PATH(std::string, login, "login"),
             PATH(std::string, repo, "repo"))
};
```

使用方式：

```cpp
auto gh = std::make_shared<GitHubClient>();

// 同步
auto res = gh->get_user_sync("octocat");

// 异步
gh->get_repo("octocat", "Hello-World", [](auto ec, auto res) {
    if (!ec) {
        fmt::print("Stars: {}\n", res.body());
    }
});
```

### 参数标签

| 标签 | 用途 | 示例 |
|------|------|------|
| `PATH(Type, Name, Key)` | 路径参数，替换 URL 中的 `:key` | `PATH(std::string, id, "id")` |
| `QUERY(Type, Name, Key)` | 查询字符串参数 | `QUERY(int, page, "page")` |
| `HEADER(Type, Name, Key)` | 自定义请求头 | `HEADER(std::string, token, "Authorization")` |
| `BODY(Type, Name)` | 请求体（自动序列化为 JSON） | `BODY(boost::json::object, data)` |

### Oat++ 风格 API 定义

使用 `KHTTPD_API_CLIENT` 或 `KHTTPD_API_CLIENT_POOL` 宏以类 Oat++ 的声明式风格定义客户端：

```cpp
// 单 host 客户端
KHTTPD_API_CLIENT(GitHubClient, "https://api.github.com")
    API_CALL(http::verb::get, "/users/:login", get_user,
             PATH(std::string, login, "login"))
    API_CALL(http::verb::get, "/users/:login/repos", list_repos,
             PATH(std::string, login, "login"),
             QUERY(int, page, "page"))
KHTTPD_API_CLIENT_END()
```

在类体内部可以直接使用 `API_CALL` 宏，自动生成：
- `get_user(login, callback)` — 异步方法
- `get_user_sync(login)` — 同步方法

### 多 Host + 权重分发

使用 `KHTTPD_API_CLIENT_POOL` 定义多 host 客户端，请求按权重分配到不同后端：

```cpp
KHTTPD_API_CLIENT_POOL(GitHubClient,
    KHTTPD_HOST("https://api.github.com", 3)         // 权重 3 (60%)
    KHTTPD_HOST("https://api-backup.github.com", 2)  // 权重 2 (40%)
)
    API_CALL(http::verb::get, "/users/:login", get_user,
             PATH(std::string, login, "login"))
KHTTPD_API_CLIENT_END()
```

每次请求时，客户端会按权重随机选择一个 host 发起请求。

### 宏参考

| 宏 | 说明 |
|----|------|
| `KHTTPD_API_CLIENT(Name, Host)` | 定义继承自 `HttpClient` 的类，自动设置单 host 基础 URL |
| `KHTTPD_API_CLIENT_POOL(Name, ...)` | 定义继承自 `HttpClient` 的类，使用多 host 池 + 权重分发 |
| `KHTTPD_HOST(Url, Weight)` | Host 池配置项，指定 URL 和权重 |
| `KHTTPD_API_CLIENT_END()` | 结束类定义 |
| `API_CALL(METHOD, PATH, NAME, ...)` | 在类体内使用，生成异步 + 同步 API 方法 |
| `verb_from_string("GET")` | 将字符串转换为 `http::verb` 枚举值 |

---

## 流式 HTTP 客户端

`HttpClient` 返回 `response<string_body>`，适合普通 API。大请求或响应应使用 `HttpClientStream`，由调用方提供固定大小缓冲区：

```cpp
#include "framework/client/http_client_stream.hpp"

auto stream = std::make_shared<HttpClientStream>(ioc);
HttpClientStream::RequestHead head{http::verb::post, "/upload", 11};
head.chunked(true);

stream->async_start("http://storage.internal/upload", std::move(head),
  [stream](beast::error_code ec) {
    // async_write_some(...) -> async_finish_request(...)
    // -> async_read_response_head(...) -> async_read_some(...)
  });
```

同一方向必须串行调用：等待当前 read/write 回调后再提交下一块。这既限制 in-flight 数据，也让 TCP 自然提供背压。调用 `cancel()` 会取消解析、连接和未完成 I/O。

流式客户端同时支持 `http://` 和 `https://`，TLS 不会回退到全量缓存。默认构造函数使用系统信任库并校验证书；私有 CA 或测试环境可以使用 `HttpClientStream(ioc, ssl_context)` 注入自定义 context。

缓冲和流式客户端都会连续消费上游 `100 Continue`、`103 Early Hints` 等 1xx 响应，并只返回最终响应。HEAD 请求在最终响应头后即完成，即使响应包含非零 `Content-Length` 也不会等待正文。

---

## WebSocket 客户端

### 基本使用

```cpp
#include "framework/client/websocket_client.hpp"

namespace net = boost::asio;

auto ws = std::make_shared<khttpd::framework::client::WebsocketClient>();

// 设置回调
ws->set_on_message([](const std::string& msg) {
    fmt::print("Received: {}\n", msg);
});

// 需要保留 text/binary/control 类型时使用帧回调。
ws->set_on_frame([](const WebsocketFrame& frame) {
    if (frame.type == WebsocketFrameType::binary) {
        fmt::print("binary bytes: {}\n", frame.payload.size());
    }
});

// 请求子协议；连接后可读取服务端最终选择的协议。
ws->set_subprotocols({"chat.v1", "chat.v2"});

ws->set_on_error([](beast::error_code ec) {
    if (ec != boost::asio::error::operation_aborted) {
        fmt::print(stderr, "WS Error: {}\n", ec.message());
    }
});

ws->set_on_close([]() {
    fmt::print("Connection closed\n");
});

// 连接
ws->connect("wss://echo.websocket.org", [ws](beast::error_code ec) {
    if (!ec) {
        fmt::print("protocol: {}\n", ws->negotiated_subprotocol());
        fmt::print("Connected!\n");
    }
});
```

### 发送消息

```cpp
// 发送文本消息（线程安全）
ws->send("Hello, server!");

// 发送多条消息（自动排队）
ws->send("Message 1");
ws->send("Message 2");
ws->send("Message 3");

// 发送二进制帧，payload 可以包含 NUL 字节。
ws->send({WebsocketFrameType::binary, std::string("\x00\x01", 2)});
```

### 完整示例：Echo 客户端

```cpp
class EchoClient {
public:
    EchoClient(net::io_context& ioc) : ws_(std::make_shared<WebsocketClient>(ioc)) {
        ws_->set_on_message([this](const std::string& msg) {
            fmt::print("Echo: {}\n", msg);
            echo_count_++;
            if (echo_count_ < 3) {
                ws_->send(fmt::format("Hello #{}", echo_count_ + 1));
            } else {
                ws_->close();
            }
        });

        ws_->set_on_close([]() {
            fmt::print("Done!\n");
        });

        ws_->set_on_error([](beast::error_code ec) {
            if (ec != boost::asio::error::operation_aborted) {
                fmt::print(stderr, "Error: {}\n", ec.message());
            }
        });
    }

    void start() {
        ws_->connect("wss://echo.websocket.org", [this](beast::error_code ec) {
            if (!ec) {
                ws_->send("Hello #1");
            }
        });
    }

private:
    std::shared_ptr<WebsocketClient> ws_;
    int echo_count_ = 0;
};
```

### URL 格式

| 前缀 | 说明 |
|------|------|
| `ws://host:port/path` | 普通 WebSocket |
| `wss://host:port/path` | TLS 加密 WebSocket |

### 自定义握手头

```cpp
ws->set_header("Authorization", "Bearer token123");
ws->connect("wss://api.example.com/ws", ...);
```
