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
| `set_max_buffered_request_body_size(bytes)` | `void` | 设置普通缓冲路由的请求体上限；默认 16 MiB，仅影响之后建立的连接 |
| `get_max_buffered_request_body_size()` | `std::uint64_t` | 获取当前普通缓冲路由请求体上限 |
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
| `peer_endpoint()` | `const std::optional<tcp::endpoint>&` | TCP 真实对端；不受 `X-Forwarded-For` 伪造影响 |
| `peer_address()` | `std::optional<ip::address>` | TCP 真实对端地址，适合可信代理判断和 IP 限流 |

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
| `stream(path, method, handler)` | 注册在读取完整 body 前分发的流式路由 |
| `async_route(path, method, handler)` | 注册异步路由；handler 完成时调用一次 `complete()` |

`handler` 签名：`void(HttpContext&)`

流式 `handler` 签名：

```cpp
void(HttpContext&,
     std::shared_ptr<HttpRequestStream>,
     std::shared_ptr<HttpResponseStream>,
     HttpStreamComplete)
```

普通 handler 仍使用 `string_body`，超过 Server 配置上限会返回 413。流式路由使用固定缓冲区读取，不受该缓冲上限约束。

### 强类型 JSON 路由

`get/post/put/del/options` 还接受返回值非 `void` 的强类型 callable：

```cpp
router.post("/users", [](const CreateUserRequest& request) -> HttpResult<UserResponse> {
    return HttpResult<UserResponse>::created({1001, request.name})
        .header("Location", "/users/1001");
});
```

支持的 Controller 成员签名（也支持 `const` 成员函数）：

```cpp
Response method(const Request&);
Response method(const Request&, HttpContext&);
```

`Response` 可以是 JSON 可序列化裸类型、`HttpResult<T>` 或 `HttpResult<void>`。裸类型自动返回 HTTP 200。
第二个 `HttpContext&` 参数用于读取 path/query/header/cookie 和拦截器属性。请求体必须是 `application/json`
或 `application/*+json`；媒体类型或 JSON/DTO 转换失败时返回 HTTP 400，并且不会调用业务 handler。

Controller 可使用：

```cpp
KHTTPD_TYPED_ROUTE(post, "/users", create_user);
```

原有 `KHTTPD_ROUTE`、`void(HttpContext&)` 和所有路由分发行为保持不变。强类型路由仍执行相同的前置/后置拦截器，
不能替代鉴权拦截器。

### HttpResult

| API | 说明 |
|------|------|
| `HttpResult<T>(status, body)` | 指定状态和 JSON 响应体 |
| `HttpResult<T>::ok(body)` | 创建 HTTP 200 响应 |
| `HttpResult<T>::created(body)` | 创建 HTTP 201 响应 |
| `HttpResult<void>::no_content()` | 创建无响应体的 HTTP 204 响应 |
| `result.header(name, value)` | 增加经过校验的响应头 |

响应头拒绝 CR/LF/NUL、其他控制字符、非法字段名，以及 `Content-Length`、`Transfer-Encoding`、`Connection`
等由服务器管理的 framing/hop-by-hop 字段，避免响应拆分和消息边界冲突。

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
| `map_exception<E>(mapper)` | 将异常 `E` 映射为裸 JSON 响应或 `HttpResult<T>` |
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
| `add_handler(path, on_open, on_message, on_close, on_error)` | 注册 WebSocket 生命周期处理器；支持 `/gateway/:target` 动态参数，最后一个参数可匹配多层路径 |
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
| `frame` | `WebsocketFrame` | 当前帧的类型、原始 payload、关闭码和关闭原因 |
| `session_weak_ptr` | `weak_ptr<WebsocketSession>` | 会话的弱引用 |

### 方法

| 方法 | 说明 |
|------|------|
| `send(msg, is_text)` | 发送消息给客户端 |
| `send(frame)` | 按 `WebsocketFrameType` 发送 text、binary、ping、pong 或 close 帧 |
| `handshake()` | 获取原始 target、路径、重复 headers、query 参数和客户端请求的 subprotocol 列表 |
| `get_header(name)` / `get_headers(name)` | 大小写不敏感地读取握手 header；复数版本保留重复字段 |
| `get_query_param(key)` | 读取握手查询参数 |
| `get_path_param(key)` | 读取 WebSocket 动态路由参数 |
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
| `async_handle_request(ctx, complete)` | 调用同步 `handle_request` | 异步前置检查；完成时调用一次 `complete(result)` |
| `handle_response(ctx)` | 空 | 响应生成后，按添加**逆序**执行 |

返回 `Stop` 时中断后续拦截器和路由处理器，直接执行后置拦截器。
HTTP 与 WebSocket Upgrade 都会执行同一条前置拦截器链。

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

### 强类型异常映射

```cpp
router.map_exception<ValidationError>([](const ValidationError& error) {
    return HttpResult<ErrorResponse>(
        http::status::unprocessable_entity,
        {"VALIDATION_FAILED", error.what()});
});
```

也可以抛出 `HttpException(status, json_body, internal_message)`，并通过 `.header(name, value)` 添加经过相同安全校验的响应头。
`internal_message` 只写服务端日志，不进入 JSON 响应。未匹配的 `std::exception` 默认返回：

```json
{"code":"INTERNAL_SERVER_ERROR","message":"Internal server error"}
```

异常响应会先清除 handler 已经写入的部分响应，避免错误路径泄露残留的 header 或 body。旧的
`ExceptionDispatcher`、`ExceptionHandler<E>` 和自定义 unknown handler 保持兼容。

---

## OpenAPI 3.1 文档

头文件：

```cpp
#include "framework/router/openapi.hpp"
```

### 数据类型

```cpp
struct OpenApiInfo {
    std::string title = "khttpd API";
    std::string version = "1.0.0";
};

struct RouteDescriptor {
    std::string path;
    boost::beast::http::verb method;
    std::optional<boost::json::value> request_schema;
    std::optional<boost::json::value> response_schema;
};
```

`HttpRouter::route_descriptors()` 返回不含 handler、正则表达式、拦截器和异常映射器的副本，调用方无法借此修改路由器内部状态。

### 生成和导出

```cpp
boost::json::object generate_openapi(
    const HttpRouter& router,
    const OpenApiInfo& info = {});

void export_openapi(
    const HttpRouter& router,
    const std::string& output_path,
    const OpenApiInfo& info = {});
```

输出固定为 OpenAPI 3.1.0。路径中的 `:id` 转换为 `{id}`；由于当前路由匹配规则允许最后一个动态参数跨 `/`，该参数带有 `x-khttpd-greedy: true`。旧式、异步和流式路由生成 method/path/parameter/response 骨架；强类型路由还生成 JSON request body 和 response schema。

Boost.Describe DTO 可自动展开字段：

```cpp
struct CreateRequest { std::string name; int age; };
BOOST_DESCRIBE_STRUCT(CreateRequest, (), (name, age))
```

仅通过自定义 `tag_invoke` 序列化且没有 Boost.Describe 元数据的类型会退化为 `{ "type": "object" }`，不会猜测字段。`std::optional<T>` 字段不进入 `required`；字符串、布尔、整数、浮点和 `std::vector<T>` 会生成对应 schema。

文件输出采用确定性路径/方法顺序并以换行结尾。空路径、无法打开或无法完整写入会抛出异常。API 不替调用方限制目标目录，因此不要把未经授权的网络输入直接作为 `output_path`。

### 运行时文档路由

```cpp
void install_openapi_routes(
    HttpRouter& router,
    const OpenApiInfo& info = {},
    const std::string& spec_path = "/openapi.json",
    const std::string& docs_path = "/docs",
    bool enabled = true);
```

先注册业务路由，再调用该函数。它会安装只读 JSON 与 HTML 入口，并从生成文档中隐藏自身。两个路径必须是互不相同的绝对字面路径，且不能与已有 GET 路由冲突；冲突会抛出 `std::invalid_argument`，不会覆盖业务 handler。路由仍走标准 session、前置/后置 interceptor 和授权流程，不存在单独的越权 dispatch 通道。

传入 `enabled = false` 时函数不注册 `/openapi.json` 或 `/docs`，可用于按环境、租户或权限策略手动关闭运行时文档；离线 `export_openapi` 不受此开关影响。

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

## HTTP 流式 API

### HttpRequestStream

| 方法 | 说明 |
|------|------|
| `async_read_some(buffer, callback)` | 将下一段请求体读入调用方缓冲区；回调参数为 `(ec, bytes, done)` |
| `cancel_read()` | 只取消请求体读取；响应通道仍可发送，连接随后以非 keep-alive 结束 |
| `cancel()` | 取消读取并关闭对应连接 |

同一个方向必须等待前一次回调完成后再发起下一次读取。

### HttpResponseStream

| 方法 | 说明 |
|------|------|
| `async_start(head, callback)` | 发送响应头；未指定 Content-Length/Chunked 时自动使用 chunked |
| `async_write_some(buffer, callback)` | 发送一段响应体 |
| `async_finish(callback)` | 完成响应体并写入终止块（如需要） |
| `cancel_request_body()` | 只取消配对的请求体读取，保留响应通道 |
| `cancel()` | 取消下游响应 |

### HttpClientStream

| 方法 | 说明 |
|------|------|
| `async_start(url, head, callback)` | 连接上游并发送请求头 |
| `async_write_some(buffer, callback)` | 发送一段请求体 |
| `async_finish_request(callback)` | 完成请求体 |
| `async_read_response_head(callback)` | 读取上游响应头 |
| `async_read_some(buffer, callback)` | 固定缓冲区读取响应体 |
| `cancel()` | 取消解析、连接和未完成 I/O |

流式客户端支持 `http://` 和 `https://`，TLS 传输仍使用同一套固定缓冲 serializer/parser。默认 context 校验系统信任库；也可通过 `HttpClientStream(ssl_context)` 或 `HttpClientStream(ioc, ssl_context)` 注入私有 CA 配置。
客户端会跳过连续的 100/103 等 informational response，向调用方交付最终响应；HEAD 按响应头完成，不等待 `Content-Length` 指示的正文。

### HttpProxySession

`HttpProxySession` 把入站请求流、上游 `HttpClientStream` 和下游响应流串联起来。请求和响应方向都遵循“读一块、写一块、写完再读下一块”，从而形成自然背压。

```cpp
auto proxy = std::make_shared<client::HttpProxySession>(
    request_stream, response_stream, 64 * 1024);
proxy->start(upstream_url, std::move(request_head), complete_callback);
```

代理会过滤 hop-by-hop headers，透明保留 Range、Content-Range、Accept-Ranges、ETag 等端到端字段，并在任一侧失败时取消其余方向。

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
| `set_subprotocols(protocols)` | 设置 `Sec-WebSocket-Protocol` 请求列表 |
| `negotiated_subprotocol()` | 返回握手响应中服务端最终选择的子协议 |
| `set_on_message(handler)` | 设置消息回调 |
| `set_on_frame(handler)` | 设置保留 text/binary/control 类型的帧回调 |
| `set_on_error(handler)` | 设置错误回调 |
| `set_on_close(handler)` | 设置关闭回调 |
| `send(frame)` | 发送带明确类型和关闭信息的 `WebsocketFrame` |
