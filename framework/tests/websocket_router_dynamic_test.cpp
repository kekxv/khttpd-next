#include "framework/router/websocket_router.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <thread>

namespace fw = khttpd::framework;

namespace
{
  fw::WebsocketContext context_for(std::string path)
  {
    return fw::WebsocketContext(std::weak_ptr<fw::WebsocketSession>{}, std::move(path));
  }
}

TEST(WebsocketRouterDynamicTest, MultipleParametersKeepFinalParameterGreedy)
{
  fw::WebsocketRouter router;
  std::string tenant;
  std::string target;
  router.add_handler("/gateway/:tenant/:target", [&](fw::WebsocketContext& ctx)
  {
    tenant = ctx.get_path_param("tenant").value_or("");
    target = ctx.get_path_param("target").value_or("");
  });
  auto ctx = context_for("/gateway/acme/orders/ws/v2");
  router.dispatch_open(ctx.path, ctx);
  EXPECT_EQ(tenant, "acme");
  EXPECT_EQ(target, "orders/ws/v2");
}

TEST(WebsocketRouterDynamicTest, LiteralRegexCharactersAreMatchedLiterally)
{
  fw::WebsocketRouter router;
  bool called = false;
  router.add_handler("/socket.v1/:target", [&](fw::WebsocketContext&) { called = true; });
  auto wrong = context_for("/socketXv1/orders");
  router.dispatch_open(wrong.path, wrong);
  EXPECT_FALSE(called);
  auto exact = context_for("/socket.v1/orders");
  router.dispatch_open(exact.path, exact);
  EXPECT_TRUE(called);
}

TEST(WebsocketRouterDynamicTest, StaticRouteWinsRegardlessOfRegistrationOrder)
{
  fw::WebsocketRouter router;
  std::string selected;
  router.add_handler("/gateway/:target", [&](fw::WebsocketContext&) { selected = "dynamic"; });
  router.add_handler("/gateway/health", [&](fw::WebsocketContext&) { selected = "static"; });
  auto ctx = context_for("/gateway/health");
  router.dispatch_open(ctx.path, ctx);
  EXPECT_EQ(selected, "static");
}

TEST(WebsocketRouterDynamicTest, HandlerCanReplaceItselfDuringDispatch)
{
  fw::WebsocketRouter router;
  int version = 0;
  router.add_handler("/hot", [&](fw::WebsocketContext&)
  {
    version = 1;
    router.add_handler("/hot", [&](fw::WebsocketContext&) { version = 2; });
  });
  auto first = context_for("/hot");
  router.dispatch_open(first.path, first);
  EXPECT_EQ(version, 1);
  auto second = context_for("/hot");
  router.dispatch_open(second.path, second);
  EXPECT_EQ(version, 2);
}

TEST(WebsocketRouterDynamicTest, ConcurrentDispatchAndHotUpdateRemainSafe)
{
  fw::WebsocketRouter router;
  std::atomic<int> calls{0};
  router.add_handler("/gateway/:target", [&](fw::WebsocketContext&) { ++calls; });
  std::thread dispatcher([&]
  {
    for (int i = 0; i < 500; ++i)
    {
      auto ctx = context_for("/gateway/service/ws");
      router.dispatch_open(ctx.path, ctx);
    }
  });
  for (int i = 0; i < 100; ++i)
    router.add_handler("/gateway/:target", [&](fw::WebsocketContext&) { ++calls; });
  dispatcher.join();
  EXPECT_EQ(calls, 500);
}
