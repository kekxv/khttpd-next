#ifndef KHTTPD_FRAMEWORK_CRON_JOB_HPP
#define KHTTPD_FRAMEWORK_CRON_JOB_HPP

#include <string>
#include <functional>
#include <memory>
#include <ctime>
#include <atomic>
#include <chrono>
#include <boost/asio.hpp>
#include <spdlog/spdlog.h>
#include "croncpp.hpp"
#include "io_context_pool.hpp"

namespace khttpd::framework
{
  class CronJob : public std::enable_shared_from_this<CronJob>
  {
  public:
    explicit CronJob(const std::string& expression)
      : timer_(IoContextPool::instance().get_io_context())
        , expression_(expression)
        , is_running_(false)
    {
      try
      {
        cron_expr_ = cron::make_cron(expression);
      }
      catch (const std::exception& e)
      {
        spdlog::error("[CronJob] Invalid expression '{}': {}", expression, e.what());
        throw;
      }
    }

    virtual ~CronJob()
    {
    }

    /**
     * @brief 启动任务
     * @param delay_ms 延迟启动时间（毫秒），默认为 0（立即计算下一次执行时间）
     */
    void start(std::chrono::milliseconds delay_ms = std::chrono::milliseconds(0))
    {
      bool expected = false;
      if (is_running_.compare_exchange_strong(expected, true))
      {
        if (delay_ms.count() > 0)
        {
          // 延迟启动逻辑
          timer_.expires_after(delay_ms);
          auto self = shared_from_this();
          timer_.async_wait([this, self](const boost::system::error_code& ec)
          {
            if (!ec && is_running_)
            {
              schedule_next(); // 延迟结束后，开始正常的 cron 调度
            }
          });
        }
        else
        {
          // 立即启动
          schedule_next();
        }
      }
    }

    void stop()
    {
      is_running_ = false;
      timer_.cancel();
    }

    // 判断当前是否在运行状态
    bool is_running() const { return is_running_; }

  protected:
    virtual void run() = 0;

  private:
    void schedule_next()
    {
      if (!is_running_) return;

      auto now_time_t = std::time(nullptr);
      std::time_t next_time_t = cron::cron_next(cron_expr_, now_time_t);
      auto next_time_point = std::chrono::system_clock::from_time_t(next_time_t);

      timer_.expires_at(next_time_point);

      auto self = shared_from_this();

      timer_.async_wait([this, self](const boost::system::error_code& ec)
      {
        if (ec == boost::asio::error::operation_aborted) return;
        if (!is_running_) return;

        if (ec)
        {
          spdlog::error("[CronJob] Timer error: {}", ec.message());
          return;
        }

        try
        {
          this->run();
        }
        catch (const std::exception& e)
        {
          spdlog::error("[CronJob] Task exception: {}", e.what());
        }

        if (is_running_)
        {
          schedule_next();
        }
      });
    }

  private:
    boost::asio::system_timer timer_;
    std::string expression_;
    cron::cronexpr cron_expr_;
    std::atomic<bool> is_running_;
  };
}

#endif
