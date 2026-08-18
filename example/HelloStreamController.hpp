//
// Created by Caesar on 2025/5/30.
//

#ifndef HELLOSTREAMCONTROLLER_HPP
#define HELLOSTREAMCONTROLLER_HPP
#include "controller/http_controller.hpp"

class HelloStreamController : public khttpd::framework::BaseController<HelloStreamController>
{
public:
  static std::shared_ptr<HelloStreamController> create() { return std::make_shared<HelloStreamController>(); }

  std::shared_ptr<BaseController> register_routes(khttpd::framework::HttpRouter& router) override
  {
    KHTTPD_DOCUMENTED_ROUTE(get, "/stream/:size", handle_stream,
                            {"Stream response chunks", "Streams up to 100 JSON chunks for the requested size."});

    return shared_from_this();
  }

private:
  size_t num_chunks_to_send_ = 0;

  void handle_stream(khttpd::framework::HttpContext& ctx)
  {
    std::string num_str = ctx.get_path_param("size").value_or("0");
    num_chunks_to_send_ = std::stoul(num_str);
    if (num_chunks_to_send_ > 100)
    {
      num_chunks_to_send_ = 100;
    }
    ctx.set_status(boost::beast::http::status::ok);
    ctx.set_content_type("application/json");
    auto do_stream_chunk = [this, num_str](auto& session, const auto& writeHandler)
    {
      for (int i = 0; i < num_chunks_to_send_; i++)
      {
        auto json = fmt::format(R"("id": {}, "url": "/stream/{}", "args": , "headers": {})",
                                i, num_str, "\n");
        if (!writeHandler(json)) { break; }
      }
    };
    ctx.chunked(do_stream_chunk);
  }
};

#endif //HELLOSTREAMCONTROLLER_HPP
