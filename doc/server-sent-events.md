# 异步 Server-Sent Events

khttpd 的 SSE 支持建立在异步流式 HTTP 传输之上，不占用阻塞线程。服务端通过
`SseSession` 顺序写入事件，客户端通过 `SseClient` 增量读取事件；普通响应、
`HttpContext::chunked()` 和 WebSocket 的处理路径不受影响。

## 服务端

使用 `HttpRouter::sse` 注册 GET 路由：

```cpp
#include "sse/sse_session.hpp"

router.sse("/events",
  [](HttpContext&, std::shared_ptr<sse::SseSession> session)
  {
    session->on_close([](boost::system::error_code ec) {
      // 从业务侧的订阅者集合移除连接。
    });

    session->send({
      "config",                     // event
      R"({"name":"app.yaml"})",   // data，可包含多行
      "42",                         // id
      3000                           // retry，毫秒
    });
    session->send_comment("heartbeat");
  });
```

`router.sse` 的可选第三个参数用于设置单连接待发送队列的字节上限，默认 1 MiB：

```cpp
router.sse("/events", handler, 256 * 1024);
```

队列即将超过上限时 `send()` 返回 `false`，业务可丢弃慢订阅者或触发全量同步，
不会继续占用内存。SSE 路由在 handler 前运行普通的全局同步/异步拦截器，鉴权或
权限拦截器返回 `Stop` 时不会发送 SSE 响应头，也不会执行 handler。

响应会包含 `Content-Type: text/event-stream`、`Cache-Control: no-cache` 和
`X-Accel-Buffering: no`，正文使用 HTTP 分块传输。`send()` 和
`send_comment()` 可从不同线程调用，内部拥有待写入字符串并按 FIFO 顺序串行写入，
自然继承底层 socket 的背压。

如果路由处理函数返回后仍要继续推送，应用必须在订阅者集合中持有
`std::shared_ptr<SseSession>`。`close()` 会等待已排队事件写完再发送结束块；
`cancel()` 会立即中断连接。两者最终都只触发一次 `on_close`。

心跳建议使用注释帧，例如 `send_comment("heartbeat")`；心跳周期和空闲连接管理由
业务服务决定。

## 客户端

```cpp
#include "client/sse_client.hpp"

auto subscription = std::make_shared<client::SseClient>(ioc); // 默认单事件上限 1 MiB
subscription->connect(
  "https://config.internal/events",
  {
    {"Authorization", "Bearer internal-token"},
    {"Last-Event-ID", "41"},
  },
  [](const sse::SseEvent& event) {
    // event.event、event.data、event.id、event.retry
  },
  [](boost::system::error_code ec) {
    // 按业务策略决定是否重连。
  });
```

客户端要求上游返回 HTTP 200，且 Content-Type 必须为 `text/event-stream`
（允许带 charset 等参数）。解析器支持任意网络分片、CRLF/LF、多行 `data`、注释、
`event`、`id` 和纯数字 `retry`；无效的 `retry` 会被忽略。

调用方需要在订阅期间持有 `SseClient`。显式调用 `cancel()` 会关闭传输，并以
`operation_aborted` 调用一次关闭回调。框架不自动重连：调用方可以结合事件的
`retry`、指数退避和 `Last-Event-ID` 实现符合具体服务需求的恢复策略。
构造函数的可选字节上限用于限制未完成行或单个事件；超限只会关闭当前订阅，并以
`message_size` 完成关闭回调，不会终止进程。

## 选择 SSE 还是 WebSocket

- 服务器向客户端持续单向推送配置、实例或通知时，优先使用 SSE。
- 需要全双工消息、二进制帧或自定义控制帧时，使用 WebSocket。
- 需要上传和下载同时承受背压时，使用双向 HTTP 流式接口。
