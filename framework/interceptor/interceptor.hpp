#ifndef KHTTPD_FRAMEWORK_INTERCEPTOR_INTERCEPTOR_HPP_
#define KHTTPD_FRAMEWORK_INTERCEPTOR_INTERCEPTOR_HPP_

#include "context/http_context.hpp"
#include <functional>

namespace khttpd::framework
{
  enum class InterceptorResult
  {
    Continue,
    Stop // Intercepted, do not proceed to next interceptor or route handler
  };

  class Interceptor
  {
  public:
    using RequestCompletion = std::function<void(InterceptorResult)>;
    virtual ~Interceptor() = default;

    /**
     * @brief Pre-request handler.
     * @param ctx The HTTP context.
     * @return InterceptorResult::Continue to proceed, InterceptorResult::Stop to stop processing.
     */
    virtual InterceptorResult handle_request(HttpContext& ctx)
    {
      return InterceptorResult::Continue;
    }

    // Override this for remote authentication or other asynchronous policy
    // checks. Invoke complete exactly once; it may be invoked from any thread.
    // The default preserves existing synchronous interceptors.
    virtual void async_handle_request(HttpContext& ctx, RequestCompletion complete)
    {
      complete(handle_request(ctx));
    }

    /**
     * @brief Post-response handler.
     * @param ctx The HTTP context.
     */
    virtual void handle_response(HttpContext& ctx)
    {
    }
  };
}

#endif // KHTTPD_FRAMEWORK_INTERCEPTOR_INTERCEPTOR_HPP_
