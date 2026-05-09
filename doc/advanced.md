# 高级功能

## 拦截器（Interceptors）

拦截器在请求到达路由处理**前**和响应生成**后**执行，支持链式组合。

### 实现拦截器

```cpp
struct AuthInterceptor : public khttpd::framework::Interceptor {
    InterceptorResult handle_request(HttpContext& ctx) override {
        auto auth = ctx.get_header("Authorization");
        if (!auth || auth->empty()) {
            ctx.set_status(boost::beast::http::status::unauthorized);
            ctx.set_body("Unauthorized");
            return InterceptorResult::Stop;
        }
        // 将用户信息存入上下文，供后续 handler 使用
        ctx.set_attribute("auth_token", *auth);
        return InterceptorResult::Continue;
    }

    void handle_response(HttpContext& ctx) override {
        // 添加全局响应头
        ctx.set_header("X-Powered-By", "khttpd");
    }
};
```

### 注册拦截器

```cpp
server->add_interceptor(std::make_shared<AuthInterceptor>());
server->add_interceptor(std::make_shared<LoggingInterceptor>());
```

### 执行顺序

```
Request → Interceptor1.handle_request → Interceptor2.handle_request → Handler
        ← Interceptor2.handle_response ← Interceptor1.handle_response ← Response
```

- **前置拦截器**：按注册**正序**执行
- **后置拦截器**：按注册**逆序**执行（洋葱模型）
- 任一前置返回 `Stop` → 跳过剩余前置和 handler → 执行全部后置

### 上下文数据传递

```cpp
// 前置拦截器
ctx.set_attribute("user_id", std::string("user-456"));

// 路由 handler
auto uid = ctx.get_attribute_as<std::string>("user_id");
// uid.value() == "user-456"
```

---

## 异常处理

### ExceptionDispatcher（推荐）

```cpp
auto dispatcher = std::make_shared<khttpd::framework::ExceptionDispatcher>();

dispatcher->on<std::runtime_error>([](const std::runtime_error& e, HttpContext& ctx) {
    ctx.set_status(boost::beast::http::status::internal_server_error);
    ctx.set_body(fmt::format("Server Error: {}", e.what()));
});

dispatcher->on<int>([](const int code, HttpContext& ctx) {
    // throw 404; 等整型异常
    ctx.set_status(static_cast<boost::beast::http::status>(code));
});

dispatcher->on<const char*>([](const char* const msg, HttpContext& ctx) {
    ctx.set_status(boost::beast::http::status::bad_request);
    ctx.set_body(std::string("Error: ") + msg);
});

server->get_http_router().add_exception_handler(dispatcher);
```

### 自定义异常处理器

```cpp
class MyException : public std::exception {
    std::string msg_;
public:
    explicit MyException(std::string msg) : msg_(std::move(msg)) {}
    const char* what() const noexcept override { return msg_.c_str(); }
};

class MyExceptionHandler : public khttpd::framework::ExceptionHandler<MyException> {
    void handle(const MyException& e, HttpContext& ctx) override {
        ctx.set_status(boost::beast::http::status::unprocessable_entity);
        ctx.set_body(e.what());
    }
};

router.add_exception_handler(std::make_shared<MyExceptionHandler>());
```

### 未知异常兜底

```cpp
router.set_unknown_exception_handler([](HttpContext& ctx) {
    ctx.set_status(boost::beast::http::status::service_unavailable);
    ctx.set_body("An unexpected error occurred");
});
```

---

## WebSocket

### 基本用法

```cpp
auto& ws_router = server->get_websocket_router();

ws_router.add_handler(
    "/chat",
    // on_open
    [](WebsocketContext& ctx) {
        ctx.send("Welcome to the chat!");
    },
    // on_message
    [](WebsocketContext& ctx) {
        ctx.send("Echo: " + ctx.message, ctx.is_text);
    },
    // on_close
    [](WebsocketContext& ctx) {
        // 清理资源
    },
    // on_error
    [](WebsocketContext& ctx) {
        fmt::print(stderr, "WS Error: {}\n", ctx.error_code.message());
    }
);
```

### 广播消息

```cpp
// 向指定 session 发送消息
WebsocketSession::send_message(session_id, "Hello!", true);

// 批量发送
std::vector<std::string> ids = {"id1", "id2", "id3"};
WebsocketSession::send_message(ids, "Broadcast message", true);
```

### Controller 方式注册

```cpp
class ChatController : public khttpd::framework::BaseController<ChatController> {
    std::string base_path() override { return "/chat"; }

    std::shared_ptr<BaseController> register_routes(HttpRouter& router) override {
        KHTTPD_ROUTE(get, "", handle_upgrade_hint);
        return shared_from_this();
    }

    std::shared_ptr<BaseController> register_routes(WebsocketRouter& router) override {
        KHTTPD_WSROUTE("", on_open, on_message, on_close, on_error);
        return shared_from_this();
    }

    void handle_upgrade_hint(HttpContext& ctx) {
        ctx.set_status(boost::beast::http::status::upgrade_required);
        ctx.set_header(boost::beast::http::field::upgrade, "websocket");
        ctx.set_body("WebSocket endpoint");
    }

    void on_open(WebsocketContext& ctx) { /* ... */ }
    void on_message(WebsocketContext& ctx) { /* ... */ }
    void on_close(WebsocketContext& ctx) { /* ... */ }
    void on_error(WebsocketContext& ctx) { /* ... */ }
};

ChatController::create()->register_routes(ws_router);
```

---

## 分块流式响应

```cpp
router.get("/stream/:count", [](HttpContext& ctx) {
    int count = std::stoi(ctx.get_path_param("count").value_or("10"));

    auto stream = [count](HttpContext& ctx, const WriteHandler& write) {
        for (int i = 0; i < count; i++) {
            auto chunk = fmt::format("Chunk {}\n", i);
            if (!write(chunk)) break;  // 客户端断开时 write 返回 false
        }
    };

    ctx.chunked(stream);
});
```

### 写入控制

`WriteHandler` 返回 `false` 时停止写入（客户端已断开）。

---

## Cron 定时任务

### Lambda 任务

```cpp
auto& scheduler = khttpd::framework::CronScheduler::instance();

// 每分钟执行一次
scheduler.schedule("* * * * * *", []() {
    fmt::print("Cron tick: {}\n", std::time(nullptr));
});

// 每 5 分钟，延迟 10 秒启动
scheduler.schedule("0 */5 * * * *", []() {
    // 清理过期 session
}, std::chrono::milliseconds(10000));
```

### 继承式任务

```cpp
class CleanupJob : public khttpd::framework::CronJob {
public:
    CleanupJob() : CronJob("0 0 * * * *") {}  // 每天午夜

protected:
    void run() override {
        // 执行清理逻辑
    }
};

auto job = std::make_shared<CleanupJob>();
job->start();
```

### Cron 表达式（6 字段：秒 分 时 日 月 周）

| 表达式 | 说明 |
|--------|------|
| `* * * * * *` | 每秒 |
| `0 * * * * *` | 每分钟 |
| `0 */5 * * * *` | 每 5 分钟 |
| `0 0 * * * *` | 每小时 |
| `0 0 9 * * *` | 每天 9:00 |
| `0 0 9 * * 1-5` | 工作日 9:00 |

---

## 依赖注入（DI Container）

### 注册与解析

```cpp
auto& di = khttpd::framework::DI_Container::instance();

// 无依赖组件
di.register_component<DatabaseService>();

// 有依赖组件（自动注入构造）
di.register_component<UserRepository, DatabaseService>();
di.register_component<UserService, UserRepository>();

// 解析（单例）
auto userService = di.resolve<UserService>();
```

### 嵌套依赖

```cpp
// A → B → C
di.register_component<CService>();
di.register_component<BService, CService>();
di.register_component<AService, BService>();

auto a = di.resolve<AService>();  // 自动解析 BService → CService
```

### 组件必须继承 ComponentBase

```cpp
class MyService : public khttpd::framework::ComponentBase {
public:
    explicit MyService(std::shared_ptr<Dependency> dep) : dep_(dep) {}
    void do_something() { /* ... */ }
private:
    std::shared_ptr<Dependency> dep_;
};
```

### 循环依赖检测

```cpp
di.register_component<A, B>();
di.register_component<B, A>();

// 抛出 std::runtime_error: "Circular dependency detected"
di.resolve<A>();
```

---

## Cookie 操作

### 读取 Cookie

```cpp
auto session_id = ctx.get_cookie("session_id");  // std::optional<std::string>
auto all_users = ctx.get_cookies("user");         // std::vector<std::string>
```

### 设置 Cookie

```cpp
// 简单 cookie
ctx.set_cookie("foo", "bar");

// 完整选项
CookieOptions opts;
opts.max_age = 3600;        // 1 小时
opts.path = "/api";
opts.domain = "example.com";
opts.secure = true;         // 仅 HTTPS
opts.http_only = true;      // 禁止 JS 访问
opts.same_site = "Strict";
ctx.set_cookie("user", "123", opts);

// 删除 cookie（max_age = 0）
CookieOptions delete_opts;
delete_opts.max_age = 0;
ctx.set_cookie("user", "", delete_opts);
```

> **注意**: Cookie 的 key 和 value 不能包含 `;`, `,`, `\r`, `\n`。key 还不能包含 `=`。设置包含这些字符的 cookie 会被拒绝。
