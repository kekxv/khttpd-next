#ifndef KHTTPD_FRAMEWORK_CLIENT_IO_CONTEXT_POOL_HPP
#define KHTTPD_FRAMEWORK_CLIENT_IO_CONTEXT_POOL_HPP

#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <thread>
#include <vector>
#include <memory>
#include <mutex>
#include <chrono>
#include <atomic>

namespace khttpd::framework
{
  class IoContextPool
  {
  public:
    // 获取单例实例
    static IoContextPool& instance(unsigned int num_threads = 0)
    {
      static IoContextPool instance{num_threads};
      return instance;
    }

    // 获取共享的 io_context
    boost::asio::io_context& get_io_context()
    {
      return ioc_;
    }

    // 获取当前运行的线程数量
    size_t get_thread_count() const
    {
      return threads_.size();
    }

    ~IoContextPool()
    {
      shutdown();
    }

    void stop()
    {
      // 快速路径：重置 work guard 并通知 io_context 停止
      // 不在此函数中 join 线程，避免信号处理线程中 join 自身导致崩溃
      if (!stopped_.exchange(true))
      {
        work_guard_.reset();
        ioc_.stop();
      }
    }

    // 等待所有工作线程结束（由析构函数调用，不在信号上下文中）
    void shutdown()
    {
      if (!stopped_.exchange(true))
      {
        work_guard_.reset();
        ioc_.stop();
      }

      // 等待线程结束 — 只在析构路径调用，不在信号处理中
      for (auto& t : threads_)
      {
        if (t.joinable())
        {
          t.join();
        }
      }
      threads_.clear();
    }

  private:
    explicit IoContextPool(unsigned int count = std::thread::hardware_concurrency())
      : work_guard_(boost::asio::make_work_guard(ioc_))
    {
      if (count <= 0) count = 1;

      threads_.reserve(count * 2);

      for (unsigned int i = 0; i < count; ++i)
      {
        threads_.emplace_back([this]()
        {
          ioc_.run();
        });
      }
    }

    boost::asio::io_context ioc_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard_;
    std::vector<std::thread> threads_;
    std::atomic<bool> stopped_{false};
  };
}
#endif // KHTTPD_FRAMEWORK_CLIENT_IO_CONTEXT_POOL_HPP
