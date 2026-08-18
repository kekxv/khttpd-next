//
// Created by Caesar on 2025/5/30.
//

#ifndef HELLOCONTROLLER_HPP
#define HELLOCONTROLLER_HPP
#include "controller/http_controller.hpp"

class HelloController : public khttpd::framework::BaseController<HelloController>
{
private:
  std::string base_path() override
  {
    return "/hello";
  }

public:
  static std::shared_ptr<HelloController> create() { return std::make_shared<HelloController>(); }

  std::shared_ptr<BaseController> register_routes(khttpd::framework::HttpRouter& router) override
  {
    KHTTPD_DOCUMENTED_ROUTE(get, "/hello", handle_hello,
                            {"Controller greeting", "Returns a greeting from a controller-managed route."});

    return shared_from_this();
  }

private:
  void handle_hello(khttpd::framework::HttpContext& ctx)
  {
    std::string name = ctx.get_query_param("name").value_or("Guest");
    ctx.set_status(boost::beast::http::status::ok);
    ctx.set_content_type("text/plain");
    ctx.set_body(fmt::format("Hello, {}!", name));
  }
};

#endif //HELLOCONTROLLER_HPP
