#ifndef KHTTPD_FRAMEWORK_EXCEPTION_EXCEPTION_HANDLER_HPP_
#define KHTTPD_FRAMEWORK_EXCEPTION_EXCEPTION_HANDLER_HPP_

#include "context/http_context.hpp"

#include <exception>
#include <functional>
#include <vector>
#include <type_traits>

namespace khttpd::framework
{
  class ExceptionHandlerBase
  {
  public:
    virtual ~ExceptionHandlerBase() = default;
    
    /**
     * @brief Tries to handle the exception.
     * @param eptr The exception pointer to handle.
     * @param ctx The HTTP context.
     * @return true if the exception was handled, false otherwise.
     */
    virtual bool try_handle(std::exception_ptr eptr, HttpContext& ctx)
    {
        return false;
    }
  };

  /**
   * @brief A helper class that dispatches exceptions to registered handlers.
   * Allows registering handlers for different exception types in a single instance.
   */
  class ExceptionDispatcher : public ExceptionHandlerBase
  {
  public:
    template <typename E>
    void on(std::function<void(const E&, HttpContext&)> handler)
    {
      handlers_.push_back([handler](std::exception_ptr eptr, HttpContext& ctx) -> bool
      {
        try
        {
          std::rethrow_exception(eptr);
        }
        catch (typename std::conditional<std::is_pointer<E>::value || std::is_fundamental<E>::value, E, const E&>::type e)
        {
          handler(e, ctx);
          return true;
        }
        catch (...)
        {
          return false;
        }
      });
    }

    bool try_handle(std::exception_ptr eptr, HttpContext& ctx) override
    {
      for (const auto& handler : handlers_)
      {
        if (handler(eptr, ctx))
        {
          return true;
        }
      }
      return false;
    }

  private:
    using Delegate = std::function<bool(std::exception_ptr, HttpContext&)>;
    std::vector<Delegate> handlers_;
  };

  template <typename E>
  class ExceptionHandler : public ExceptionHandlerBase
  {
  public:
    bool try_handle(std::exception_ptr eptr, HttpContext& ctx) override
    {
      try
      {
        std::rethrow_exception(eptr);
      }
      catch (const E& e)
      {
        handle(e, ctx);
        return true;
      }
      catch (...)
      {
        return false;
      }
    }

    virtual void handle(const E& e, HttpContext& ctx) = 0;
  };
}

#endif // KHTTPD_FRAMEWORK_EXCEPTION_EXCEPTION_HANDLER_HPP_
