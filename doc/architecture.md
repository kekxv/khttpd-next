# 架构指南

## 概览

khttpd 是基于 Boost.Beast 和 Boost.Asio 构建的 C++ HTTP/WebSocket 服务器框架，采用 Bazel 构建系统。框架设计目标：简洁的路由 API、异步 I/O 高并发、完整的中间件与异常处理支持。

## 核心架构图

```
┌─────────────────────────────────────────────────────────┐
│                       Server                            │
│  ┌──────────────────┐   ┌────────────────────────────┐ │
│  │   HttpRouter     │   │     WebsocketRouter        │ │
│  │  - 路由匹配       │   │  - 路径匹配                │ │
│  │  - 拦截器链       │   │  - 生命周期分发             │ │
│  │  - 异常分发       │   │                            │ │
│  └────────┬─────────┘   └─────────────┬──────────────┘ │
│           │                           │                │
│  ┌────────┴─────────┐   ┌─────────────┴──────────────┐ │
│  │   HttpSession    │◄──┤  WebSocket Session         │ │
│  │  - HTTP 请求解析  │   │  - WS 握手                 │ │
│  │  - 静态文件服务    │   │  - 消息收发                │ │
│  │  - 响应序列化      │   │  - 广播发送                │ │
│  └────────┬─────────┘   └────────────────────────────┘ │
│           │                                            │
│  ┌────────┴─────────┐                                  │
│  │  HttpContext     │   WebsocketContext               │
│  │  - 请求/响应封装  │   - 消息/连接状态                │
│  │  - 参数提取       │   - 扩展属性                     │
│  │  - Cookie/JSON   │                                  │
│  └──────────────────┘                                  │
└─────────────────────────────────────────────────────────┘
         │                                    │
┌────────┴─────────┐              ┌───────────┴──────────┐
│  IoContextPool   │              │   CronScheduler      │
│  - 线程池管理     │              │   DI Container       │
│  - io_context    │              │   Exception Handler  │
└──────────────────┘              │   Interceptor        │
                                  │   HttpClient/WS      │
                                  └──────────────────────┘
```

## 请求处理流程

```
Client Request
      │
      ▼
┌─────────────┐
│  Acceptor   │  接受 TCP 连接，分配到 Strand
└──────┬──────┘
       │
       ▼
┌─────────────┐
│ HttpSession │  async_read_header 先读取请求行和 headers
└──────┬──────┘
       │
       ▼
┌──────────────────┐
│ Header Route     │  流式路由 → buffer_body 固定缓冲读取
│ Decision         │  普通路由 → 在可配置上限内累计 body
└──────┬───────────┘
       │
       ▼
┌──────────────────┐
│ Pre-Interceptor  │  链式执行，可中断
│     Chain        │  任一返回 Stop → 跳到 Post-Interceptor
└──────┬───────────┘
       │ Continue
       ▼
┌─────────────┐
│  Router     │  按优先级匹配路由
│  Dispatch   │  执行 handler
└──────┬──────┘
       │
       ▼
┌──────────────────┐
│ Post-Interceptor │  逆序执行
│     Chain        │
└──────┬───────────┘
       │
       ▼
┌─────────────┐
│  Response   │  async_write 发送响应
│  (send)     │  支持 keep-alive 多请求
└─────────────┘
```

## WebSocket 升级流程

```
HTTP Request (Upgrade header)
      │
      ▼
┌─────────────┐
│ HttpSession │  检测 is_upgrade()
│  on_read    │
└──────┬──────┘
       │
       ▼
┌─────────────────────┐
│ WebSocket Session   │  释放 socket，创建 WS 会话
│  run_handshake      │  async_accept 完成握手
└──────┬──────────────┘
       │
       ▼
┌─────────────────────┐
│ dispatch_open       │  触发 on_open 回调
│                     │  注册到全局 session 注册表
└──────┬──────────────┘
       │
       ▼
┌─────────────────────┐
│  do_read loop       │  async_read → dispatch_message
│                     │  消息循环直到连接关闭
└─────────────────────┘
```

## 关键设计决策

### 1. 线程模型

- 使用 **Boost.Asio strand** 保证单个连接的串行化
- 多个 strand 分配到线程池 (`IoContextPool`) 实现并发
- **不需要** 在 handler 中加锁（同一连接的 handler 在 strand 上串行执行）

### 2. 路由匹配

- 使用 **正则表达式** 解析动态路径参数
- 按**特异性排序**：字面段多的路由优先，同数量下动态段少的优先
- HTTP 与 WebSocket 路由均支持 `:param` 动态参数和特异性排序
- WebSocket 最后一个动态参数可捕获剩余多层路径，例如 `/gateway/:target` 匹配 `/gateway/orders/ws/v1`
- WebSocket 路由表使用共享锁读取、独占锁更新；热更新只影响后续分发

### 3. 响应发送

- 普通响应：`beast::async_write` 异步发送
- Chunked 流式响应：通过 `HttpContext::chunked()` 启用，内部将同步写入转换为异步写链
- 双向 HTTP 流：`request_parser<buffer_body>` 与 `response<buffer_body>` 使用调用方固定缓冲区；每次 write 完成后才继续 read，形成背压
- 静态文件：使用 Beast 的 `file_body` 零拷贝发送

### 4. 内存管理

- `HttpSession` / `WebsocketSession` 由 `shared_ptr` 管理
- 普通 JSON/form 路由保留完整 body，但由 `Server::set_max_buffered_request_body_size()` 控制上限（默认 16 MiB）
- 流式路由、客户端和代理使用固定大小缓冲区，内存不随单个 body 大小线性增长
- 流对象持有对应会话，异步操作完成或取消前不会悬空
- 拦截器和异常处理器存储为 `shared_ptr` 在路由器中

### 5. WebSocket 帧与握手元数据

- `WebsocketFrame` 统一表示 text、binary、ping、pong 和 close 帧
- binary payload 使用 `std::string` 作为字节容器，但不会转换为文本，类型由 `frame.type` 明确保留
- `WebsocketHandshakeRequest` 保留原始 target、path、重复 headers、query 参数和 subprotocol 请求列表
- 写操作通过每连接串行队列执行，避免并发 `async_write`

### 6. 静态文件安全

- 使用 `boost::filesystem::canonical()` 规范化路径，防止 `../` 目录遍历
- 规范化后校验路径仍在 `web_root` 内
- 目录请求自动尝试 `index.html`

## 扩展点

| 扩展方式 | 说明 |
|----------|------|
| **拦截器** | 实现 `Interceptor` 接口，注册到 `HttpRouter` |
| **异常处理器** | 继承 `ExceptionHandler<E>` 或使用 `ExceptionDispatcher` |
| **Controller** | 继承 `BaseController<Derived>`，用 `KHTTPD_ROUTE` 注册路由 |
| **Cron 任务** | 调用 `CronScheduler::instance().schedule()` |
| **DI 组件** | 继承 `ComponentBase`，注册到 `DI_Container` |

## 目录结构

```
framework/
├── server.hpp/cpp              # 主服务器：acceptor、信号处理
├── io_context_pool.hpp         # IO 线程池（单例）
├── context/
│   ├── http_context.hpp/cpp    # HTTP 请求/响应上下文
│   ├── http_request_stream.hpp # 入站请求流接口
│   ├── http_response_stream.hpp # 下游响应流接口
│   └── websocket_context.hpp/cpp # WebSocket 上下文
├── router/
│   ├── http_router.hpp/cpp     # HTTP 路由匹配与分发
│   └── websocket_router.hpp/cpp # WebSocket 路由与生命周期
├── controller/
│   └── http_controller.hpp     # CRTP Controller + 路由宏
├── client/
│   ├── http_client.hpp/cpp     # HTTP 客户端（同步/异步）
│   ├── http_client_stream.hpp/cpp # 固定缓冲 HTTP 流客户端
│   ├── http_proxy_session.hpp/cpp # 双向代理泵与背压
│   ├── websocket_client.hpp/cpp # WebSocket 客户端
│   └── macros.hpp              # API_CALL 宏
├── interceptor/
│   └── interceptor.hpp         # 拦截器接口
├── exception/
│   └── exception_handler.hpp   # 异常处理器
├── cron/
│   ├── CronJob.hpp             # Cron 任务基类
│   ├── CronScheduler.hpp       # Cron 调度器（单例）
│   └── cronacci.hpp            # Cron 表达式解析
├── di/
│   └── di_container.hpp        # 依赖注入容器（单例）
├── session/
│   └── http_session.hpp/cpp    # HTTP 连接会话
└── websocket/
    └── websocket_session.hpp/cpp # WebSocket 连接会话
```
