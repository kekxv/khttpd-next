# khttpd 文档索引

## 文档列表

| 文档 | 说明 |
|------|------|
| [快速开始指南](quick-start.md) | 10 分钟搭建第一个 khttpd 服务 |
| [API 参考文档](api-reference.md) | 完整 API 方法签名和参数说明 |
| [架构指南](architecture.md) | 框架设计、请求流程、线程模型、扩展点 |
| [高级功能](advanced.md) | 拦截器、异常处理、WebSocket、Cron、DI 容器、Cookie |
| [HTTP 与 WebSocket 客户端](http-client.md) | 内置客户端 API、API_CALL 宏、WebSocket 客户端 |

## 按主题查找

### 入门
- 环境要求、安装 Bazel → [快速开始](quick-start.md#环境要求)
- 创建第一个项目 → [快速开始](quick-start.md#创建项目)
- 构建与运行 → [快速开始](quick-start.md#构建并运行)

### 路由
- HTTP 路由注册 → [API 参考](api-reference.md#httprouter)
- 路径参数语法 → [API 参考](api-reference.md#路由语法)
- 路由优先级 → [API 参考](api-reference.md#路由优先级)
- Controller 模式 → [API 参考](api-reference.md#basecontroller)
- WebSocket 路由 → [高级功能](advanced.md#websocket)

### 请求/响应
- 获取查询参数 → [API 参考](api-reference.md#参数提取)
- 获取路径参数 → [API 参考](api-reference.md#参数提取)
- 解析 JSON → [API 参考](api-reference.md#json-解析)
- 解析表单 → [API 参考](api-reference.md#表单与文件上传)
- 文件上传 → [API 参考](api-reference.md#表单与文件上传)
- 设置响应 → [API 参考](api-reference.md#响应设置)
- 分块流式响应 → [高级功能](advanced.md#分块流式响应)
- Cookie 操作 → [高级功能](advanced.md#cookie-操作)

### 中间件
- 拦截器 → [高级功能](advanced.md#拦截器interceptors)
- 异常处理 → [高级功能](advanced.md#异常处理)
- 上下文数据传递 → [高级功能](advanced.md#上下文数据传递)

### 定时任务
- Cron 调度器 → [高级功能](advanced.md#cron-定时任务)
- Cron 表达式 → [高级功能](advanced.md#cron-表达式6-字段秒-分-时-日-月-周)

### 依赖注入
- 注册与解析 → [高级功能](advanced.md#依赖注入di-container)
- 嵌套依赖 → [高级功能](advanced.md#嵌套依赖)
- 循环依赖检测 → [高级功能](advanced.md#循环依赖检测)

### 客户端
- HTTP 客户端 → [HTTP 客户端](http-client.md#http-客户端)
- Oat++ 风格 API 定义 → [HTTP 客户端](http-client.md#oat-风格-api-定义)
- 多 Host 权重分发 → [HTTP 客户端](http-client.md#多-host-权重分发)
- API_CALL 宏 → [HTTP 客户端](http-client.md#api_call-宏自动生成客户端方法)
- WebSocket 客户端 → [HTTP 客户端](http-client.md#websocket-客户端)

### 架构
- 请求处理流程 → [架构指南](architecture.md#请求处理流程)
- WebSocket 升级流程 → [架构指南](architecture.md#websocket-升级流程)
- 线程模型 → [架构指南](architecture.md#1-线程模型)
- 扩展点 → [架构指南](architecture.md#扩展点)
