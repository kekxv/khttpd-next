#include "framework/client/http_proxy_session.hpp"

#include <gtest/gtest.h>
#include <boost/asio.hpp>

namespace fw = khttpd::framework;
namespace client = khttpd::framework::client;
namespace http = boost::beast::http;
namespace net = boost::asio;

namespace
{
  class ObservableRequestStream final : public fw::HttpRequestStream
  {
  public:
    bool read = false;
    bool cancelled = false;

    void async_read_some(net::mutable_buffer, ReadCallback callback) override
    {
      read = true;
      callback({}, 0, true);
    }
    void cancel() override { cancelled = true; }
  };

  class ObservableResponseStream final : public fw::HttpResponseStream
  {
  public:
    bool started = false;
    bool cancelled = false;

    void async_start(ResponseHead, Callback callback) override
    {
      started = true;
      callback({});
    }
    void async_write_some(net::const_buffer, Callback callback) override { callback({}); }
    void async_finish(Callback callback) override { callback({}); }
    void cancel() override { cancelled = true; }
  };
}

TEST(HttpProxySessionEdgeTest, UnsupportedUpstreamSchemeCancelsBothDownstreamSides)
{
  net::io_context ioc;
  auto inbound = std::make_shared<ObservableRequestStream>();
  auto downstream = std::make_shared<ObservableResponseStream>();
  auto proxy = std::make_shared<client::HttpProxySession>(ioc, inbound, downstream, 17);
  client::HttpClientStream::RequestHead head{http::verb::post, "/", 11};
  boost::system::error_code result;
  bool completed = false;

  proxy->start("ftp://unsupported.test/upload", std::move(head),
               [&](boost::system::error_code ec) { result = ec; completed = true; });
  ioc.run();

  EXPECT_TRUE(completed);
  EXPECT_EQ(result, make_error_code(boost::system::errc::operation_not_supported));
  EXPECT_TRUE(inbound->cancelled);
  EXPECT_TRUE(downstream->cancelled);
  EXPECT_FALSE(inbound->read);
  EXPECT_FALSE(downstream->started);
}

TEST(HttpProxySessionEdgeTest, ExplicitCancelCancelsInboundAndDownstream)
{
  net::io_context ioc;
  auto inbound = std::make_shared<ObservableRequestStream>();
  auto downstream = std::make_shared<ObservableResponseStream>();
  auto proxy = std::make_shared<client::HttpProxySession>(ioc, inbound, downstream);

  proxy->cancel();

  EXPECT_TRUE(inbound->cancelled);
  EXPECT_TRUE(downstream->cancelled);
}
