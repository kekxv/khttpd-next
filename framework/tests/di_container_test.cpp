#include "gtest/gtest.h"
#include "di/di_container.hpp"
#include <string>
#include <atomic> // For tracking construction/destruction

using namespace khttpd::framework;

// Helper to track construction/destruction for tests
std::atomic<int> s_DependencyA_count{0};
std::atomic<int> s_DependencyB_count{0};
std::atomic<int> s_MainComponent_count{0};

// ===========================================
// 测试用组件类
// ===========================================

class DependencyA : public ComponentBase
{
public:
  bool constructed = false;

  DependencyA()
  {
    constructed = true;
    s_DependencyA_count++;
    // spdlog::debug("DependencyA constructed. Count: {}", s_DependencyA_count);
  }

  ~DependencyA() override
  {
    s_DependencyA_count--;
    // spdlog::debug("DependencyA destructed. Count: {}", s_DependencyA_count);
  }
};

class DependencyB : public ComponentBase
{
public:
  bool constructed = false;
  std::shared_ptr<DependencyA> depA;

  explicit DependencyB(std::shared_ptr<DependencyA> a) : depA(a)
  {
    constructed = true;
    s_DependencyB_count++;
    // spdlog::debug("DependencyB constructed. Count: {}", s_DependencyB_count);
  }

  ~DependencyB() override
  {
    s_DependencyB_count--;
    // spdlog::debug("DependencyB destructed. Count: {}", s_DependencyB_count);
  }

  [[nodiscard]] std::shared_ptr<DependencyA> getDepA() const
  {
    return depA;
  }
};

class MainComponent : public ComponentBase
{
public:
  bool constructed = false;
  std::shared_ptr<DependencyB> depB;

  explicit MainComponent(std::shared_ptr<DependencyB> b) : depB(b)
  {
    constructed = true;
    s_MainComponent_count++;
    // spdlog::debug("MainComponent constructed. Count: {}", s_MainComponent_count);
  }

  ~MainComponent() override
  {
    s_MainComponent_count--;
    // spdlog::debug("MainComponent destructed. Count: {}", s_MainComponent_count);
  }

  [[nodiscard]] std::shared_ptr<DependencyB> getDepB() const
  {
    return depB;
  }
};

// ===========================================
// GTest 测试夹具 (Test Fixture)
// ===========================================

class DIContainerTest : public ::testing::Test
{
protected:
  DI_Container& container = DI_Container::instance();

  void SetUp() override
  {
    // 在每个测试开始前清理容器，确保测试独立
    container.clear();
    // 重置计数器
    s_DependencyA_count = 0;
    s_DependencyB_count = 0;
    s_MainComponent_count = 0;
  }

  void TearDown() override
  {
    // 确保所有 shared_ptr 在测试结束时被释放，从而触发析构
    // GTest会自动在测试结束后销毁局部变量，导致shared_ptr引用计数归零
    // 但这里为了明确性，我们可以在需要时再次clear
    container.clear(); // 确保单例缓存也被清空，以便析构
    ASSERT_EQ(s_DependencyA_count, 0) << "DependencyA instances not fully destructed.";
    ASSERT_EQ(s_DependencyB_count, 0) << "DependencyB instances not fully destructed.";
    ASSERT_EQ(s_MainComponent_count, 0) << "MainComponent instances not fully destructed.";
  }
};

// ===========================================
// 测试用例
// ===========================================

// 测试 DI_Container 自身是否是单例
TEST_F(DIContainerTest, IsSingleton)
{
  DI_Container& container1 = DI_Container::instance();
  DI_Container& container2 = DI_Container::instance();
  ASSERT_EQ(&container1, &container2);
}

// 测试注册和解析没有依赖的组件
TEST_F(DIContainerTest, RegisterAndResolveNoDependencies)
{
  container.register_component<DependencyA>();

  auto a = container.resolve<DependencyA>();
  ASSERT_NE(nullptr, a);
  ASSERT_TRUE(a->constructed);
  ASSERT_EQ(s_DependencyA_count, 1); // 应该只创建了一个实例
}

// 测试解析一个未注册的组件是否抛出异常
TEST_F(DIContainerTest, ResolveNonExistentComponentThrows)
{
  ASSERT_THROW(container.resolve<DependencyA>(), std::runtime_error);
}

// 测试依赖解析是否工作
TEST_F(DIContainerTest, DependencyResolutionWorks)
{
  container.register_component<DependencyA>();
  container.register_component<DependencyB, DependencyA>();

  auto b = container.resolve<DependencyB>();
  ASSERT_NE(nullptr, b);
  ASSERT_TRUE(b->constructed);
  ASSERT_NE(nullptr, b->getDepA());
  ASSERT_TRUE(b->getDepA()->constructed);

  // 验证 DependencyA 也是单例，即 b 内部的 DependencyA 和直接解析的 DependencyA 是同一个实例
  ASSERT_EQ(b->getDepA().get(), container.resolve<DependencyA>().get());

  ASSERT_EQ(s_DependencyA_count, 1);
  ASSERT_EQ(s_DependencyB_count, 1);
}

// 测试嵌套依赖解析是否工作
TEST_F(DIContainerTest, NestedDependencyResolutionWorks)
{
  container.register_component<DependencyA>();
  container.register_component<DependencyB, DependencyA>();
  container.register_component<MainComponent, DependencyB>();

  auto mainComp = container.resolve<MainComponent>();
  ASSERT_NE(nullptr, mainComp);
  ASSERT_TRUE(mainComp->constructed);

  ASSERT_NE(nullptr, mainComp->getDepB());
  ASSERT_TRUE(mainComp->getDepB()->constructed);

  ASSERT_NE(nullptr, mainComp->getDepB()->getDepA());
  ASSERT_TRUE(mainComp->getDepB()->getDepA()->constructed);

  // 验证所有依赖都是单例，即它们都是同一个实例
  ASSERT_EQ(mainComp->getDepB().get(), container.resolve<DependencyB>().get());
  ASSERT_EQ(mainComp->getDepB()->getDepA().get(), container.resolve<DependencyA>().get());

  ASSERT_EQ(s_DependencyA_count, 1);
  ASSERT_EQ(s_DependencyB_count, 1);
  ASSERT_EQ(s_MainComponent_count, 1);
}

// 测试组件是否被正确地作为单例管理
TEST_F(DIContainerTest, ComponentIsSingleton)
{
  container.register_component<DependencyA>();

  auto a1 = container.resolve<DependencyA>();
  auto a2 = container.resolve<DependencyA>();

  ASSERT_EQ(a1.get(), a2.get()); // 验证地址相同
  ASSERT_EQ(s_DependencyA_count, 1); // 应该只构造了一次
}

// 测试重新注册组件（覆盖）是否正常（应该给出警告但不抛异常）
TEST_F(DIContainerTest, OverwriteRegistrationWarning)
{
  container.register_component<DependencyA>();
  // GTest usually captures or redirects warning logs; this only verifies it does not crash.
  container.register_component<DependencyA>();

  // 验证仍能解析
  auto a = container.resolve<DependencyA>();
  ASSERT_NE(nullptr, a);
}

// 测试当一个组件的依赖未注册时是否抛出异常
TEST_F(DIContainerTest, ResolveThrowsForMissingNestedDependency)
{
  // 只注册 DependencyB，不注册 DependencyA
  container.register_component<DependencyB, DependencyA>();

  // 尝试解析 DependencyB 应该失败，因为 DependencyA 未注册
  ASSERT_THROW(container.resolve<DependencyB>(), std::runtime_error);
}

// 测试共享指针的生命周期管理
TEST_F(DIContainerTest, SharedPtrManagesLifetime)
{
  container.register_component<DependencyA>();

  std::shared_ptr<DependencyA> a_ptr;
  {
    auto temp_ptr = container.resolve<DependencyA>();
    a_ptr = temp_ptr; // 拷贝 shared_ptr，引用计数增加
    ASSERT_EQ(s_DependencyA_count, 1);
  } // temp_ptr 超出作用域，引用计数减1，但 a_ptr 仍持有，所以不会析构

  ASSERT_EQ(s_DependencyA_count, 1); // 实例仍然存在

  container.clear(); // 清空容器缓存，不再持有引用
  // 此时，如果 a_ptr 是唯一持有者，则应该析构
  // GTest的TearDown会再次clear，所以在此之前，a_ptr 可能会是唯一持有者并析构
  // 或者在a_ptr自己超出作用域后析构
}

// 确保在嵌套解析中，所有依赖只被构造一次
TEST_F(DIContainerTest, NestedResolutionOnlyConstructsOnce)
{
  container.register_component<DependencyA>();
  container.register_component<DependencyB, DependencyA>();
  container.register_component<MainComponent, DependencyB>();

  // 第一次解析 MainComponent，应该构造所有依赖一次
  auto mainComp1 = container.resolve<MainComponent>();
  ASSERT_EQ(s_DependencyA_count, 1);
  ASSERT_EQ(s_DependencyB_count, 1);
  ASSERT_EQ(s_MainComponent_count, 1);

  // 第二次解析 MainComponent，不应该再次构造任何东西
  auto mainComp2 = container.resolve<MainComponent>();
  ASSERT_EQ(s_DependencyA_count, 1);
  ASSERT_EQ(s_DependencyB_count, 1);
  ASSERT_EQ(s_MainComponent_count, 1);
  ASSERT_EQ(mainComp1.get(), mainComp2.get()); // 验证是同一个实例

  // 直接解析依赖，也应该返回已存在的单例
  auto a = container.resolve<DependencyA>();
  ASSERT_EQ(s_DependencyA_count, 1);
  ASSERT_EQ(mainComp1->getDepB()->getDepA().get(), a.get());
}

// Test circular dependency detection
class CircularA : public ComponentBase
{
public:
  explicit CircularA(std::shared_ptr<class CircularB> b) : depB(b) {}
  std::shared_ptr<CircularB> depB;
};

class CircularB : public ComponentBase
{
public:
  explicit CircularB(std::shared_ptr<CircularA> a) : depA(a) {}
  std::shared_ptr<CircularA> depA;
};

TEST_F(DIContainerTest, CircularDependencyDetection)
{
  container.register_component<CircularA, CircularB>();
  container.register_component<CircularB, CircularA>();

  // Should throw with "Circular dependency" message
  ASSERT_THROW(container.resolve<CircularA>(), std::runtime_error);
}

// Test resolve after clear throws
TEST_F(DIContainerTest, ResolveAfterClear)
{
  container.register_component<DependencyA>();
  auto a = container.resolve<DependencyA>();
  ASSERT_NE(a, nullptr);

  container.clear();

  // After clear, resolve should throw since component is no longer registered
  ASSERT_THROW(container.resolve<DependencyA>(), std::runtime_error);
}
