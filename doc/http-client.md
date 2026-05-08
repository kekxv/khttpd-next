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

ws->set_on_error([](beast::error_code ec) {
    if (ec != boost::asio::error::operation_aborted) {
        fmt::print(stderr, "WS Error: {}\n", ec.message());
    }
});

ws->set_on_close([]() {
    fmt::print("Connection closed\n");
});

// 连接
ws->connect("wss://echo.websocket.org", [](beast::error_code ec) {
    if (!ec) {
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
