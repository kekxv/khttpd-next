# 快速开始指南

本文档帮助你在 10 分钟内搭建并运行第一个 khttpd 服务。

## 环境要求

| 工具 | 最低版本 |
|------|----------|
| [Bazel](https://bazel.build/) / [Bazelisk](https://github.com/bazelbuild/bazelisk) | 7.0+ |
| C++ 编译器 | C++17 支持 (Clang 13+, GCC 9+) |
| 操作系统 | macOS, Linux |

## 安装 Bazel

推荐使用 Bazelisk（自动管理 Bazel 版本）：

```bash
# macOS
brew install bazelisk

# Linux
go install github.com/bazelbuild/bazelisk@latest
```

## 创建项目

### 1. 初始化项目目录

```bash
mkdir my-khttpd-app && cd my-khttpd-app
```

### 2. 创建 MODULE.bazel

```python
module(name = "my-khttpd-app", version = "0.1.0")

bazel_dep(name = "platforms", version = "1.0.0")
bazel_dep(name = "rules_cc", version = "0.2.13")
bazel_dep(name = "fmt", version = "12.0.0")
bazel_dep(name = "boost", version = "1.89.0.bcr.2")
bazel_dep(name = "boost.asio", version = "1.89.0.bcr.2")
bazel_dep(name = "boost.beast", version = "1.89.0.bcr.2")
bazel_dep(name = "boost.json", version = "1.89.0.bcr.2")
bazel_dep(name = "boost.filesystem", version = "1.89.0.bcr.2")
bazel_dep(name = "boost.url", version = "1.89.0.bcr.2")
bazel_dep(name = "boost.uuid", version = "1.89.0.bcr.2")
bazel_dep(name = "boringssl", version = "0.20251110.0")

http_archive = use_repo_rule("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
http_archive(
    name = "khttpd",
    strip_prefix = "khttpd-0.1.0",
    url = "https://github.com/ClangTools/khttpd/archive/refs/tags/v0.1.0.tar.gz",
)
```

### 3. 创建 BUILD.bazel

```python
load("@rules_cc//cc:defs.bzl", "cc_binary")

cc_binary(
    name = "app",
    srcs = ["main.cpp"],
    deps = ["@khttpd//framework"],
)
```

### 4. 编写 main.cpp

```cpp
#include "framework/server.hpp"
#include "framework/context/http_context.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <fmt/format.h>
#include <thread>

namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

int main()
{
    auto const address = net::ip::make_address("0.0.0.0");
    auto const port = static_cast<unsigned short>(8080);
    auto const threads = std::max<int>(1, std::thread::hardware_concurrency());

    auto server = std::make_shared<khttpd::framework::Server>(
        tcp::endpoint{address, port}, "web_root", threads);

    auto& router = server->get_http_router();

    // 简单路由
    router.get("/", [](khttpd::framework::HttpContext& ctx) {
        ctx.set_status(boost::beast::http::status::ok);
        ctx.set_content_type("text/plain");
        ctx.set_body("Hello, khttpd!");
    });

    // 路径参数
    router.get("/hello/:name", [](khttpd::framework::HttpContext& ctx) {
        auto name = ctx.get_path_param("name").value_or("World");
        ctx.set_body(fmt::format("Hello, {}!", name));
    });

    // JSON API
    router.post("/api/echo", [](khttpd::framework::HttpContext& ctx) {
        if (auto json = ctx.get_json()) {
            ctx.set_body_json(*json);
        } else {
            ctx.set_status(boost::beast::http::status::bad_request);
            ctx.set_body("Invalid JSON");
        }
    });

    server->run();
    return 0;
}
```

### 5. 构建并运行

```bash
# 构建
bazel build //:app

# 运行
bazel run //:app
```

### 6. 测试

```bash
curl http://localhost:8080/
# Hello, khttpd!

curl http://localhost:8080/hello/World
# Hello, World!

curl -X POST -H "Content-Type: application/json" \
     -d '{"msg":"hi"}' http://localhost:8080/api/echo
# {"msg":"hi"}
```

## 下一步

- [API 文档](api-reference.md) — 完整 API 参考
- [架构指南](architecture.md) — 框架设计与核心概念
- [高级功能](advanced.md) — 拦截器、异常处理、WebSocket、Cron 调度、DI 容器
- [HTTP 客户端](http-client.md) — 使用内置 HTTP / WebSocket 客户端

## 目录结构

```
my-khttpd-app/
├── MODULE.bazel      # Bazel 模块依赖
├── BUILD.bazel       # 构建目标
├── main.cpp          # 应用入口
└── web_root/         # 静态文件目录（可选）
    └── index.html
```
