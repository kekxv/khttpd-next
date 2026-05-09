# API 参考文档

## Server

### 构造函数

```cpp
Server(const tcp::endpoint& endpoint, std::string web_root, int num_threads = 1);
```

| 参数 | 类型 | 说明 |
|------|------|------|
| `endpoint` | `tcp::endpoint` | 监听地址和端口，如 `tcp::endpoint{ip::make_address("0.0.0.0"), 8080}` |
| `web_root` | `std::string` | 静态文件根目录路径。服务会自动将 `/` 下的文件作为静态资源提供 |
| `num_threads` | `int` | 工作线程数，默认 1。推荐设为 `std::thread::hardware_concurrency()` |

### 方法

| 方法 | 返回值 | 说明 |
|------|--------|------|
| `get_http_router()` | `HttpRouter&` | 获取 HTTP 路由器引用，用于注册路由 |
| `get_websocket_router()` | `WebsocketRouter&` | 获取 WebSocket 路由器引用 |
| `add_interceptor(interceptor)` | `void` | 添加全局请求/响应拦截器 |
| `run()` | `void` | 启动服务器（阻塞调用，直到收到 SIGINT/SIGTERM） |
| `stop()` | `void` | 停止服务器，关闭 acceptor 和线程池 |

### 示例

```cpp
auto server = std::make_shared<khttpd::framework::Server>(
    tcp::endpoint{net::ip::make_address("0.0.0.0"), 8080},
    "web_root",
    std::thread::hardware_concurrency()
);
server->run();
```

---

## HttpContext

请求与响应的统一上下文对象。

### 请求信息

| 方法 | 返回值 | 说明 |
|------|--------|------|
| `path()` | `const std::string&` | 请求路径（不含查询字符串） |
| `method()` | `http::verb` | HTTP 方法 |
| `body()` | `std::string` | 请求体 |
| `get_request()` | `Request&` | 原始 Beast 请求对象 |

### 参数提取

| 方法 | 返回值 | 说明 |
|------|--------|------|
| `get_query_param(key)` | `std::optional<std::string>` | 查询字符串参数，如 `?name=value` |
| `get_path_param(key)` | `std::optional<std::string>` | 路径参数，如 `/users/:id` 中的 `id` |
| `get_header(name)` | `std::optional<std::string>` | 请求头（支持 `http::field` 枚举和字符串） |
| `get_headers(name)` | `std::optional<std::vector<std::string>>` | 同名请求头列表 |

### Cookie 操作

| 方法 | 返回值 | 说明 |
|------|--------|------|
| `get_cookie(key)` | `std::optional<std::string>` | 获取单个 cookie 值 |
| `get_cookies(key)` | `std::vector<std::string>` | 获取同名 cookie 列表 |
| `set_cookie(key, value, options)` | `void` | 设置响应 cookie。`options` 为 `CookieOptions` 结构体 |

`CookieOptions` 字段：

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `max_age` | `int` | `-1` | 存活秒数。`-1`=会话 cookie，`0`=删除 cookie |
| `path` | `std::string` | `"/"` | 路径 |
| `domain` | `std::string` | `""` | 域名 |
| `secure` | `bool` | `false` | 仅 HTTPS 传输 |
| `http_only` | `bool` | `true` | 禁止 JavaScript 访问 |
| `same_site` | `std::string` | `"Lax"` | `Strict`, `Lax`, `None` |

### JSON 解析

| 方法 | 返回值 | 说明 |
|------|--------|------|
| `get_json()` | `std::optional<boost::json::value>` | 解析请求体为 JSON（自动检查 Content-Type） |

### 表单与文件上传

| 方法 | 返回值 | 说明 |
|------|--------|------|
| `get_form_param(key)` | `std::optional<std::string>` | 获取 `application/x-www-form-urlencoded` 表单字段 |
| `get_multipart_field(key)` | `std::optional<std::string>` | 获取 `multipart/form-data` 文本字段 |
| `get_uploaded_files(field)` | `const std::vector<MultipartFile>*` | 获取上传的文件列表，`nullptr` 表示字段不存在 |

`MultipartFile` 结构体：

```cpp
struct MultipartFile {
    std::string filename;     // 文件名
    std::string content_type; // MIME 类型
    std::string data;         // 文件内容
};
```

### 响应设置

| 方法 | 说明 |
|------|------|
| `set_status(status)` | 设置 HTTP 状态码 |
| `set_body(str)` | 设置响应体 |
| `set_body_json(obj, opts)` | 序列化对象为 JSON 响应体，自动设置 `Content-Type: application/json` |
| `set_body_from(obj, sp, opts)` | 使用 `boost::json::value_from` 序列化响应体 |
| `set_header(name, value)` | 设置响应头 |
| `set_content_type(type)` | 设置 Content-Type |

### 分块流式传输

| 方法 | 说明 |
|------|------|
| `chunked(handler)` | 启用 chunked transfer encoding。`handler` 签名：`void(HttpContext&, const WriteHandler&)` |

`WriteHandler` 签名：`bool(const std::string& buffer)` — 写入成功返回 `true`，写入失败或连接断开返回 `false`。

### 扩展数据（拦截器间传递）

| 方法 | 说明 |
|------|------|
| `set_attribute(key, value)` | 存储任意类型数据（`std::any`） |
| `get_attribute(key)` | 获取 `std::any` 值 |
| `get_attribute_as<T>(key)` | 获取并类型转换为 `std::optional<T>` |

---

## HttpRouter

### 路由注册

| 方法 | 说明 |
|------|------|
| `get(path, handler)` | 注册 GET 路由 |
| `post(path, handler)` | 注册 POST 路由 |
| `put(path, handler)` | 注册 PUT 路由 |
| `del(path, handler)` | 注册 DELETE 路由 |
| `options(path, handler)` | 注册 OPTIONS 路由 |

`handler` 签名：`void(HttpContext&)`

### 路由语法

| 语法 | 示例 | 匹配 |
|------|------|------|
| 静态路径 | `/api/users` | 精确匹配 |
| 动态参数 | `/users/:id` | 匹配单段路径，如 `/users/123` |
| 尾部通配 | `/files/:filepath` | 最后一个参数匹配剩余所有路径段 |

### 路由优先级

当多个路由同时匹配时，按以下规则排序：
1. 字面路径段数量多的优先
2. 字面路径段数量相同时，动态参数少的优先

例如：`/users/profile` 优先于 `/users/:id`

### 拦截器与异常处理

| 方法 | 说明 |
|------|------|
| `add_interceptor(interceptor)` | 添加拦截器 |
| `add_exception_handler(handler)` | 添加异常处理器 |
| `set_unknown_exception_handler(handler)` | 设置未知异常兜底处理器 |
| `run_pre_interceptors(ctx)` | 执行前置拦截器 |
| `run_post_interceptors(ctx)` | 执行后置拦截器（逆序） |
| `handle_exception(eptr, ctx)` | 分发异常到注册的处理器 |
| `dispatch(ctx, static_file_fun)` | 路由分发 |

---

## WebsocketRouter

### 类型定义

```cpp
using WebsocketOpenHandler   = std::function<void(WebsocketContext&)>;
using WebsocketMessageHandler = std::function<void(WebsocketContext&)>;
using WebsocketCloseHandler  = std::function<void(WebsocketContext&)>;
using WebsocketErrorHandler  = std::function<void(WebsocketContext&)>;
```

### 方法

| 方法 | 说明 |
|------|------|
| `add_handler(path, on_open, on_message, on_close, on_error)` | 注册 WebSocket 路径的所有生命周期处理器。`path` 为精确匹配（不支持动态参数） |
| `dispatch_open(path, ctx)` | 分发 open 事件 |
| `dispatch_message(path, ctx)` | 分发 message 事件 |
| `dispatch_close(path, ctx)` | 分发 close 事件 |
| `dispatch_error(path, ctx)` | 分发 error 事件 |

---

## WebsocketContext

### 公共字段

| 字段 | 类型 | 说明 |
|------|------|------|
| `id` | `std::string` | 连接唯一标识 |
| `message` | `std::string` | 接收到的消息内容（仅 message 事件有效） |
| `is_text` | `bool` | 消息是否为文本（仅 message 事件有效） |
| `error_code` | `beast::error_code` | 错误码（仅 error/close 事件有效） |
| `path` | `std::string` | 连接路径 |
| `session_weak_ptr` | `weak_ptr<WebsocketSession>` | 会话的弱引用 |

### 方法

| 方法 | 说明 |
|------|------|
| `send(msg, is_text)` | 发送消息给客户端 |
| `set_attribute(key, value)` | 存储扩展数据 |
| `get_attribute_as<T>(key)` | 获取并类型转换扩展数据 |

---

## BaseController

### 类定义

```cpp
template <typename Derived>
class BaseController : public std::enable_shared_from_this<Derived>
```

### 虚函数

| 方法 | 默认实现 | 说明 |
|------|----------|------|
| `base_path()` | `""` | 重写以设置路由前缀 |
| `register_routes(HttpRouter&)` | 纯虚 | 注册 HTTP 路由 |
| `register_routes(WebsocketRouter&)` | 空实现 | 注册 WebSocket 路由 |

### 辅助函数

| 方法 | 说明 |
|------|------|
| `bind_handler(&Class::method)` | 将成员函数绑定为路由处理器 |

### 路由宏

```cpp
KHTTPD_ROUTE(verb, path, method_name)   // HTTP 路由
KHTTPD_WSROUTE(path, ...)               // WebSocket 路由（2-5 个处理器参数）
```

---

## Interceptor

### 枚举

```cpp
enum class InterceptorResult { Continue, Stop };
```

### 虚函数

| 方法 | 默认返回 | 调用时机 |
|------|----------|----------|
| `handle_request(ctx)` | `Continue` | 路由处理前，按添加顺序执行 |
| `handle_response(ctx)` | 空 | 响应生成后，按添加**逆序**执行 |

返回 `Stop` 时中断后续拦截器和路由处理器，直接执行后置拦截器。

---

## Exception Handling

### ExceptionDispatcher

```cpp
class ExceptionDispatcher : public ExceptionHandlerBase
{
public:
    template <typename E>
    void on(std::function<void(const E&, HttpContext&)> handler);
};
```

注册多种异常类型的处理器，按注册顺序匹配。

### ExceptionHandler&lt;E&gt;

```cpp
template <typename E>
class ExceptionHandler : public ExceptionHandlerBase
{
    virtual void handle(const E& e, HttpContext& ctx) = 0;
};
```

针对单一异常类型的处理器（需继承实现）。

---

## DI Container

### 单例访问

```cpp
auto& container = khttpd::framework::DI_Container::instance();
```

### 方法

| 方法 | 说明 |
|------|------|
| `register_component<T, Args...>()` | 注册组件 `T`，依赖 `Args...`（自动注入构造） |
| `resolve<T>()` | 解析组件实例（单例） |
| `clear()` | 清空所有注册和缓存 |

### 示例

```cpp
auto& di = DI_Container::instance();
di.register_component<DatabaseService>();
di.register_component<UserRepository, DatabaseService>();
auto repo = di.resolve<UserRepository>();
```

---

## Cron Scheduler

### 单例访问

```cpp
auto& scheduler = khttpd::framework::CronScheduler::instance();
```

### 方法

| 方法 | 说明 |
|------|------|
| `schedule(expression, task, delay)` | 调度定时任务。返回 `shared_ptr<CronJob>` 句柄 |

`expression` 为 6 字段 cron 表达式（秒 分 时 日 月 周），如 `"0 */5 * * * *"`（每 5 分钟）。

### CronJob

```cpp
class CronJob {
public:
    void start(delay_ms);  // 启动任务，可选延迟
    void stop();           // 停止任务
    bool is_running();     // 是否运行中
};
```

---

## HttpClient

### 构造函数

| 构造函数 | 说明 |
|----------|------|
| `HttpClient()` | 使用全局 IO 池 + 默认 SSL |
| `HttpClient(ssl::context&)` | 全局 IO + 自定义 SSL |
| `HttpClient(io_context&)` | 自定义 IO + 默认 SSL |
| `HttpClient(io_context&, ssl::context&)` | 完全自定义 |

### 配置

| 方法 | 说明 |
|------|------|
| `set_base_url(url)` | 设置基础 URL |
| `set_default_header(key, value)` | 设置默认请求头 |
| `set_bearer_token(token)` | 设置 Bearer Token 认证 |
| `set_timeout(seconds)` | 设置超时时间 |

### 请求

| 方法 | 说明 |
|------|------|
| `request(method, path, query_params, body, headers, callback)` | 异步请求 |
| `request_sync(method, path, query_params, body, headers)` | 同步请求 |

### API_CALL 宏

```cpp
// 异步 + 同步方法自动生成
API_CALL(http::verb::get, "/users/:id", get_user,
         PATH(std::string, id),
         QUERY(std::string, filter, "filter"),
         HEADER(std::string, token, "Authorization"))
```

生成方法：
- `get_user(id, filter, token, callback)` — 异步
- `get_user_sync(id, filter, token)` — 同步

参数标签：`QUERY(Type, Name, Key)`, `PATH(Type, Name)`, `BODY(Type, Name)`, `HEADER(Type, Name, Key)`

---

## WebsocketClient

### 构造函数

| 构造函数 | 说明 |
|----------|------|
| `WebsocketClient()` | 默认 SSL |
| `WebsocketClient(io_context&)` | 指定 IO 上下文 |

### 方法

| 方法 | 说明 |
|------|------|
| `connect(url, callback)` | 连接 WebSocket（支持 `ws://` 和 `wss://`） |
| `send(message)` | 发送消息（线程安全） |
| `close()` | 关闭连接 |
| `set_header(key, value)` | 设置握手头 |
| `set_on_message(handler)` | 设置消息回调 |
| `set_on_error(handler)` | 设置错误回调 |
| `set_on_close(handler)` | 设置关闭回调 |
