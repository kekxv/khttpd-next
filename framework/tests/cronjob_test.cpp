#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>

// 包含你之前的头文件
#include "cron/CronJob.hpp"
#include "io_context_pool.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <thread>

// 引入你的头文件
#include "cron/CronScheduler.hpp"
#include "io_context_pool.hpp"

using namespace std::chrono_literals;
using namespace khttpd::framework;

// --- 测试用的辅助类 ---
class TestableCronJob : public CronJob
{
public:
  TestableCronJob(const std::string& expr)
    : CronJob(expr), run_count_(0)
  {
  }

  // 实现 run 方法
  void run() override
  {
    // 1. 增加计数
    run_count_++;

    // 2. 通知测试线程
    {
      std::lock_guard<std::mutex> lock(mutex_);
      // 只需要通知，具体逻辑由测试线程判断
    }
    cv_.notify_one();
  }

  // 辅助方法：等待任务执行 n 次
  // 返回 true 表示在超时前完成了任务，false 表示超时
  bool wait_for_runs(int expected_count, std::chrono::milliseconds timeout)
  {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this, expected_count]()
    {
      return run_count_ >= expected_count;
    });
  }

  int get_run_count() const
  {
    return run_count_;
  }

private:
  std::atomic<int> run_count_;
  std::mutex mutex_;
  std::condition_variable cv_;
};

// --- 测试套件 ---

class CronJobTest : public ::testing::Test
{
protected:
  static void SetUpTestSuite()
  {
    // 确保 IoContextPool 至少有一个线程在运行
    // 注意：单例模式下，这个池会在所有测试间共享
    IoContextPool::instance(1);
  }

  static void TearDownTestSuite()
  {
    // 测试结束后停止池（可选，视具体需求而定）
    // IoContextPool::instance().stop();
  }
};

// 测试 1: 验证无效的 Cron 表达式会抛出异常
TEST_F(CronJobTest, ThrowsOnInvalidExpression)
{
  // 这是一个错误的表达式 (只有 5 个字段，或者是乱码)
  std::string invalid_expr = "invalid cron string";

  EXPECT_THROW({
               auto job = std::make_shared<TestableCronJob>(invalid_expr);
               }, std::runtime_error); // 这里的异常类型取决于 croncpp 具体抛出什么，通常是 std::runtime_error 或 croncpp::cron_exception
}

// 测试 2: 验证任务是否能被调度和执行
TEST_F(CronJobTest, RunsScheduleCorrectly)
{
  // 设置为每秒执行一次 ("* * * * * *")
  // 注意：croncpp 能够处理秒级
  auto job = std::make_shared<TestableCronJob>("* * * * * *");

  job->start();

  // 等待任务至少执行 1 次
  // 给它 2.5 秒的时间（理论上应该在第 1 秒或第 2 秒触发）
  bool executed = job->wait_for_runs(1, std::chrono::milliseconds(2500));

  EXPECT_TRUE(executed) << "Job did not run within timeout";
  EXPECT_GE(job->get_run_count(), 1);

  job->stop();
}

// 测试 3: 验证 Stop 后不再执行
TEST_F(CronJobTest, StopPreventsFurtherExecution)
{
  auto job = std::make_shared<TestableCronJob>("* * * * * *");
  job->start();

  // 等待第 1 次
  ASSERT_TRUE(job->wait_for_runs(1, std::chrono::seconds(2)));

  // 停止
  job->stop();

  // 获取当前快照
  int count_after_stop = job->get_run_count();

  // 再等一会儿，看会不会偷偷跑
  std::this_thread::sleep_for(std::chrono::seconds(2));

  // 现在这里应该能通过了
  // 即使在 stop 瞬间正好有一次执行完成，检查3也会阻止下一次调度
  EXPECT_EQ(job->get_run_count(), count_after_stop);
}

// 测试 4: 多个任务并发
TEST_F(CronJobTest, MultipleJobs)
{
  auto job1 = std::make_shared<TestableCronJob>("* * * * * *");
  auto job2 = std::make_shared<TestableCronJob>("* * * * * *");

  job1->start();
  job2->start();

  // 等待两个任务都至少运行一次
  EXPECT_TRUE(job1->wait_for_runs(1, std::chrono::seconds(2)));
  EXPECT_TRUE(job2->wait_for_runs(1, std::chrono::seconds(2)));

  job1->stop();
  job2->stop();
}


// --- 辅助类：用于线程安全地计数和等待 ---
class AsyncCounter
{
public:
  void tick()
  {
    run_count_++;
    cv_.notify_all();
  }

  int get_count() const
  {
    return run_count_;
  }

  // 等待至少达到 expected_count 次执行
  // 返回 true 表示成功，false 表示超时
  bool wait_for_at_least(int expected_count, std::chrono::milliseconds timeout)
  {
    std::unique_lock<std::mutex> lock(mtx_);
    return cv_.wait_for(lock, timeout, [this, expected_count]()
    {
      return run_count_ >= expected_count;
    });
  }

  // 等待指定的时间，确认在此期间计数器是否变化（用于验证 Stop 和 Delay）
  // 如果计数器在 timeout 内没有增加，返回 true
  bool ensure_no_execution_for(std::chrono::milliseconds duration)
  {
    int initial = run_count_;
    std::unique_lock<std::mutex> lock(mtx_);
    // wait_for 返回 false 表示超时（即条件一直不满足），这意味着没有达到 initial + 1
    // 所以如果 wait_for 返回 false，说明没有执行，是我们想要的结果
    bool triggered = cv_.wait_for(lock, duration, [this, initial]()
    {
      return run_count_ > initial;
    });
    return !triggered;
  }

private:
  std::atomic<int> run_count_{0};
  std::mutex mtx_;
  std::condition_variable cv_;
};

// --- 测试套件 ---
class CronSchedulerTest : public ::testing::Test
{
protected:
  static void SetUpTestSuite()
  {
    // 初始化线程池，使用 2 个线程以支持并发测试
    IoContextPool::instance(2);
  }

  static void TearDownTestSuite()
  {
    IoContextPool::instance().stop();
  }

  void SetUp() override
  {
    CronScheduler::instance().stop_all();
  }

  void TearDown() override
  {
    CronScheduler::instance().stop_all();
  }
};

// 测试 1: 验证通过 Scheduler 调度的基础 Lambda 任务能正常运行
TEST_F(CronSchedulerTest, ScheduleBasic)
{
  auto counter = std::make_shared<AsyncCounter>();

  // 每秒执行一次
  // 注意：持有返回的 job 指针，否则测试函数结束时如果 pool 还在跑，任务也会跑
  auto job = CronScheduler::instance().schedule("* * * * * *", [counter]()
  {
    counter->tick();
  });

  // 等待至少执行 1 次，超时时间 2.5 秒
  ASSERT_TRUE(counter->wait_for_at_least(1, 2500ms)) << "Job failed to run within timeout";

  // 停止任务
  job->stop();
}

// 测试 2: 验证手动停止 (Stop) 功能，并测试之前的竞态条件修复
TEST_F(CronSchedulerTest, ScheduleStop)
{
  auto counter = std::make_shared<AsyncCounter>();

  // 极高频任务（每秒）
  auto job = CronScheduler::instance().schedule("* * * * * *", [counter]()
  {
    counter->tick();
  });

  // 1. 确保它跑起来了
  ASSERT_TRUE(counter->wait_for_at_least(1, 2000ms));

  // 2. 停止任务
  job->stop();

  // 3. 记录停止后的次数
  int count_after_stop = counter->get_count();

  // 4. 等待一段时间，确保它真的停了
  // 之前修复了 atomic 标志位，这里应该非常稳定
  std::this_thread::sleep_for(2000ms);

  EXPECT_EQ(counter->get_count(), count_after_stop)
        << "Job continued running after stop() was called";
}

// 测试 3: 验证延迟启动 (Delayed Start)
TEST_F(CronSchedulerTest, ScheduleDelay)
{
  auto counter = std::make_shared<AsyncCounter>();

  // 定义延迟时间：2秒
  auto delay_time = 2000ms;

  // 调度：每秒执行一次，但先延迟 2 秒
  auto job = CronScheduler::instance().schedule(
    "* * * * * *",
    [counter]() { counter->tick(); },
    delay_time
  );

  // 阶段 A: 验证在延迟期间（比如前 1 秒内），任务没有运行
  // Cron 是每秒一次，如果没有延迟，1秒内肯定会跑。
  bool no_run_early = counter->ensure_no_execution_for(1000ms);
  EXPECT_TRUE(no_run_early) << "Job ran during the delay period!";

  // 阶段 B: 验证延迟结束后，任务开始运行
  // 现在已经过了 1s，再等 2.5s (总共 3.5s)，应该能覆盖 2s 延迟 + 1s 触发
  ASSERT_TRUE(counter->wait_for_at_least(1, 2500ms))
        << "Job failed to start after delay";

  job->stop();
}

// 测试 4: 多个任务并发
TEST_F(CronSchedulerTest, MultipleTasks)
{
  auto counter1 = std::make_shared<AsyncCounter>();
  auto counter2 = std::make_shared<AsyncCounter>();

  auto job1 = CronScheduler::instance().schedule("* * * * * *", [counter1]() { counter1->tick(); });
  // job2 延迟 1 秒开始
  auto job2 = CronScheduler::instance().schedule("* * * * * *", [counter2]() { counter2->tick(); }, 1000ms);

  // 验证 job1 跑了
  EXPECT_TRUE(counter1->wait_for_at_least(1, 2000ms));

  // 验证 job2 也跑了（需要多等一会儿因为有延迟）
  EXPECT_TRUE(counter2->wait_for_at_least(1, 3000ms));

  job1->stop();
  job2->stop();
}

// 测试 5: 验证错误的表达式不会导致 Crash，而是抛出异常
TEST_F(CronSchedulerTest, InvalidExpression)
{
  EXPECT_THROW({
               CronScheduler::instance().schedule("invalid cron", [](){});
               }, std::exception);
}

TEST_F(CronSchedulerTest, UnscheduleStopsAndRemovesJob)
{
  auto counter = std::make_shared<AsyncCounter>();
  auto job = CronScheduler::instance().schedule("* * * * * *", [counter]() { counter->tick(); });

  EXPECT_EQ(CronScheduler::instance().job_count(), 1u);

  CronScheduler::instance().unschedule(job);

  EXPECT_FALSE(job->is_running());
  EXPECT_EQ(CronScheduler::instance().job_count(), 0u);
  EXPECT_TRUE(counter->ensure_no_execution_for(1200ms));
}

TEST_F(CronSchedulerTest, PruneStoppedRemovesManuallyStoppedJobs)
{
  auto job1 = CronScheduler::instance().schedule("* * * * * *", []() {});
  auto job2 = CronScheduler::instance().schedule("* * * * * *", []() {});

  EXPECT_EQ(CronScheduler::instance().job_count(), 2u);

  job1->stop();
  CronScheduler::instance().prune_stopped();

  EXPECT_EQ(CronScheduler::instance().job_count(), 1u);

  CronScheduler::instance().unschedule(job2);
  EXPECT_EQ(CronScheduler::instance().job_count(), 0u);
}
