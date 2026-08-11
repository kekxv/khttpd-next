#include "framework/client/http_client_stream.hpp"
#include "framework/client/http_client.hpp"

#include <gtest/gtest.h>
#include <boost/asio.hpp>
#include <boost/beast/ssl.hpp>
#include <array>
#include <thread>

namespace client = khttpd::framework::client;
namespace http = boost::beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace
{
  constexpr char test_certificate[] = R"PEM(-----BEGIN CERTIFICATE-----
MIIDCTCCAfGgAwIBAgIUThDTQ+lb7kmBiXurHn9Wy6vf+54wDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDgwOTE0NDcwN1oXDTI2MDgx
MDE0NDcwN1owFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEAkZayJOWhyvs9PBYbxg9pZns1OoVkr5Gs7DmDuzZSpsKW
0MrWOMCyvQVOxyOwsjJ3V4x3K4aOZMhLzBe0mewiQ+2LDTD0bqfg2mHf8x1xkwAc
WM25qiNo6yNDiFzCrxFgMRck4ejlr+YGSvKclrVoHny2Rmr8Zsvb/4SDRMIL/Qej
eOhWecl+XiE99+YQUJWbPenXHWfgGhcyvdQmiSQ3FJYv73bpsbKQ6Dw5eaTe7hlN
6M2qW39I6p7Y2I0RnzEPJ0USGXo2DLi5pzWzQDcXupC9ciH8nq2MpFvj8gZcfoEg
5ml/RAFVmLWiGyV18JIFFQAmFUTN58iw7vxKzbtvwQIDAQABo1MwUTAdBgNVHQ4E
FgQUGbjhcOG4N6JZLiLCWhxbYBM7LbQwHwYDVR0jBBgwFoAUGbjhcOG4N6JZLiLC
WhxbYBM7LbQwDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEAK37S
esIrxwdHPpKbIokLsf2/1QCVMtxNqTgFJ7GUNeg8Psu69uXbPCS3+1gRUP0385cW
goX/Ok0NVymKn+qIdZU84t+ClEFFVgrZ6onknSdRKdvEqTUsFJ5UAf/Tbq0cpOmz
bhYuFAbwwZnaLhRdsv08A8m0ql29lTMzCByVD5crvADEuXR3wqjdbbBI0zxcjK+J
pj819xaKeS37VEQ9jjwq5+w6rlaIIqy1flwsSBDp+DWB2A9nhIkwE3qfmxlOnTC9
alJ1edM4GDkgOW+q/iIrLSLq7UFl+JgQYD+jHXF3gDgjqK/riDjWYxO3TabwjcBz
d+xV3kUBYw3hrnwi7A==
-----END CERTIFICATE-----)PEM";

  constexpr char test_private_key[] = R"PEM(-----BEGIN PRIVATE KEY-----
MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQCRlrIk5aHK+z08
FhvGD2lmezU6hWSvkazsOYO7NlKmwpbQytY4wLK9BU7HI7CyMndXjHcrho5kyEvM
F7SZ7CJD7YsNMPRup+DaYd/zHXGTABxYzbmqI2jrI0OIXMKvEWAxFyTh6OWv5gZK
8pyWtWgefLZGavxmy9v/hINEwgv9B6N46FZ5yX5eIT335hBQlZs96dcdZ+AaFzK9
1CaJJDcUli/vdumxspDoPDl5pN7uGU3ozapbf0jqntjYjRGfMQ8nRRIZejYMuLmn
NbNANxe6kL1yIfyerYykW+PyBlx+gSDmaX9EAVWYtaIbJXXwkgUVACYVRM3nyLDu
/ErNu2/BAgMBAAECggEAAP4QvVEmavKO/o2dB1rcClONL5awssSws9SJihlq81GQ
wyAa2TyxCzpRyOg8oF5ZM2rU9iI+7r9xytSficwTCLkCEWczx1xUG1D+/JKHD2w5
BT7zxM3kfXPaVj/hoN1itTr16KdUh4AvK0wflqRqbwjFGlJI4a+CkqmV1n5nJASq
bb1XgM6aPJwek8MuYg8c5HY7Ul3vJyTWJKWobdG+1w0fvnWA501W7eXfBexTaCZQ
VPwpSGC4tz00XbS17defuUD8zlDJS3jETZAq5sGZMsVNoLXskaW5h3ex5aJRYNak
JC/93l/5znFLtIX2igDmis7BfO9qf9hJeeed+b/qUQKBgQDHBGRFr74poaniQIez
AXpddagd+rZ657v2GJJn+HyoyMTWt7wqDNlZgOmXmp31z4WOvs5Anb1WlaoUOM+R
m1rzuS3ool2AkkqxHo8v95xJjUKQ0iO0NsHSGhPwq/8ftEwT7mWqoz54s81wEO3J
1YRt5r1lQyrCNPDDnsGD6w4beQKBgQC7RhbDxmb9g6z4786YMXRf+lNTFIHSmaaR
mzMWrlobF0Na46OeqbkjNighWCpkekdCEqaVaYUm7gAGbiEhJgtcZTsVzwTdcTnf
qOZs+qxPCjTgQti7iyMRYwsky5BTw7/gzPYwY4lQHpS8PkQ+PzA7hTCtOnKdq82S
YO/sELmciQKBgGNs0T9zRhh8WGfc/y4xrdUlI4EesK2EOgX/Tp08qeKUsqnmjs2f
L7KkUY7YwtN8AmhG8LmdVGr+SELkAubmazDZsZLIEthZvZDxCG3ZUS35sWiyYv30
YS46sv2In+NR6rQGZKoz9dDNWvQCsRklX4ycOsBtJt5xHltMY7co5hpZAoGAU3fn
yZZybOf1fnaT5C2WqviNjugC/PTS0u8TlDZdntl9gdMYKC2JgPIwbLw5GNOPUxmw
+cMwP6uwgy0uwvGL+sB71zqP9oryuoczPLt1dT0dWB8zLlPTa3pzixDX4R3MNcvk
pqiWmQkoTcaK8BuFyeGRUoRMdY4PcACYruS9ddECgYBUAM/XcdBamOkHavC1Khut
lL7kS/l+M8K6Q2ghxEPLQzPD+gQvlUT+zjpoP9jHK3gHq3Im4bayFvmaqX+t0Piy
CQmqJob1eraZfRUHk7QnsOO0TF+s6UWf1bO3rYZBLdr15sWcAkC7e0GhfGEhSFzW
C6fgselIUr+XYmAQoT5Tqw==
-----END PRIVATE KEY-----)PEM";
}

TEST(HttpClientStreamEdgeTest, StreamsRequestAndResponseOverTls)
{
  net::io_context server_ioc;
  ssl::context server_context(ssl::context::tls_server);
  server_context.use_certificate_chain(net::buffer(test_certificate));
  server_context.use_private_key(net::buffer(test_private_key), ssl::context::file_format::pem);
  tcp::acceptor acceptor(server_ioc, {net::ip::address_v4::loopback(), 0});
  const auto port = acceptor.local_endpoint().port();
  const std::string payload(4097, 't');
  std::thread server([&]
  {
    tcp::socket socket(server_ioc);
    acceptor.accept(socket);
    boost::beast::ssl_stream<tcp::socket> tls(std::move(socket), server_context);
    tls.handshake(ssl::stream_base::server);
    boost::beast::flat_buffer buffer;
    http::request_parser<http::string_body> parser;
    parser.body_limit(8192);
    http::read(tls, buffer, parser);
    auto request = parser.release();
    http::response<http::string_body> response{http::status::ok, 11};
    response.body() = std::move(request.body());
    response.prepare_payload();
    response.keep_alive(false);
    boost::system::error_code ignored;
    http::write(tls, response, ignored);
  });

  net::io_context ioc;
  ssl::context client_context(ssl::context::tls_client);
  client_context.set_verify_mode(ssl::verify_none);
  auto stream = std::make_shared<client::HttpClientStream>(ioc, client_context);
  auto write_offset = std::make_shared<std::size_t>(0);
  auto received = std::make_shared<std::string>();
  auto write_next = std::make_shared<std::function<void()>>();
  auto read_next = std::make_shared<std::function<void()>>();
  auto read_buffer = std::make_shared<std::array<char, 11>>();
  bool completed = false;
  client::HttpClientStream::RequestHead head{http::verb::post, "/ignored", 11};
  head.content_length(payload.size());

  *read_next = [stream, received, read_buffer, read_next, &completed]
  {
    stream->async_read_some(net::buffer(*read_buffer),
      [stream, received, read_buffer, read_next, &completed]
      (boost::system::error_code ec, std::size_t size, bool done)
      {
        ASSERT_FALSE(ec) << ec.message();
        received->append(read_buffer->data(), size);
        if (!done) return (*read_next)();
        completed = true;
      });
  };
  *write_next = [stream, &payload, write_offset, write_next, read_next]
  {
    if (*write_offset == payload.size())
      return stream->async_finish_request([stream, read_next](boost::system::error_code ec)
      {
        ASSERT_FALSE(ec) << ec.message();
        stream->async_read_response_head([read_next]
          (boost::system::error_code head_ec, client::HttpClientStream::ResponseHead head)
        {
          ASSERT_FALSE(head_ec) << head_ec.message();
          EXPECT_EQ(head.result(), http::status::ok);
          (*read_next)();
        });
      });
    const auto size = std::min<std::size_t>(7, payload.size() - *write_offset);
    stream->async_write_some(net::buffer(payload.data() + *write_offset, size),
      [write_offset, size, write_next](boost::system::error_code ec)
      {
        ASSERT_FALSE(ec) << ec.message();
        *write_offset += size;
        (*write_next)();
      });
  };
  stream->async_start("https://127.0.0.1:" + std::to_string(port) + "/upload?part=1",
                      std::move(head), [write_next](boost::system::error_code ec)
  {
    ASSERT_FALSE(ec) << ec.message();
    (*write_next)();
  });
  ioc.run();
  server.join();
  EXPECT_TRUE(completed);
  EXPECT_EQ(*received, payload);
}

TEST(HttpClientStreamEdgeTest, RejectsMalformedUrl)
{
  net::io_context ioc;
  auto stream = std::make_shared<client::HttpClientStream>(ioc);
  client::HttpClientStream::RequestHead head{http::verb::get, "/", 11};
  boost::system::error_code result;
  stream->async_start("not a url", std::move(head),
                      [&](boost::system::error_code ec) { result = ec; });
  ioc.run();
  EXPECT_EQ(result, make_error_code(boost::system::errc::operation_not_supported));
}

TEST(HttpClientStreamEdgeTest, WriteBeforeStartIsAborted)
{
  net::io_context ioc;
  auto stream = std::make_shared<client::HttpClientStream>(ioc);
  const std::array<char, 1> data{'x'};
  boost::system::error_code result;
  stream->async_write_some(net::buffer(data), [&](boost::system::error_code ec) { result = ec; });
  ioc.run();
  EXPECT_EQ(result, net::error::operation_aborted);
}

TEST(HttpClientStreamEdgeTest, FinishBeforeStartIsAborted)
{
  net::io_context ioc;
  auto stream = std::make_shared<client::HttpClientStream>(ioc);
  boost::system::error_code result;
  stream->async_finish_request([&](boost::system::error_code ec) { result = ec; });
  ioc.run();
  EXPECT_EQ(result, net::error::operation_aborted);
}

TEST(HttpClientStreamEdgeTest, HeadSkipsBodyAfterConsecutiveInformationalResponses)
{
  net::io_context server_ioc;
  tcp::acceptor acceptor(server_ioc, {net::ip::address_v4::loopback(), 0});
  const auto port = acceptor.local_endpoint().port();
  std::thread server([&]
  {
    tcp::socket socket(server_ioc);
    acceptor.accept(socket);
    boost::beast::flat_buffer request_buffer;
    http::request<http::empty_body> request;
    http::read(socket, request_buffer, request);
    const std::string wire =
      "HTTP/1.1 100 Continue\r\n\r\n"
      "HTTP/1.1 103 Early Hints\r\nLink: </style.css>; rel=preload\r\n\r\n"
      "HTTP/1.1 200 OK\r\nContent-Length: 1234\r\nConnection: close\r\n\r\n";
    net::write(socket, net::buffer(wire));
  });

  net::io_context ioc;
  auto stream = std::make_shared<client::HttpClientStream>(ioc);
  client::HttpClientStream::RequestHead request{http::verb::head, "/", 11};
  bool done = false;
  stream->async_start("http://127.0.0.1:" + std::to_string(port) + "/", std::move(request),
    [stream, &done](boost::system::error_code ec)
    {
      ASSERT_FALSE(ec) << ec.message();
      stream->async_finish_request([stream, &done](boost::system::error_code finish_ec)
      {
        ASSERT_FALSE(finish_ec) << finish_ec.message();
        stream->async_read_response_head([stream, &done]
          (boost::system::error_code head_ec, client::HttpClientStream::ResponseHead head)
        {
          ASSERT_FALSE(head_ec) << head_ec.message();
          EXPECT_EQ(head.result(), http::status::ok);
          EXPECT_EQ(head[http::field::content_length], "1234");
          std::array<char, 8> body{};
          stream->async_read_some(net::buffer(body), [&done]
            (boost::system::error_code read_ec, std::size_t size, bool body_done)
          {
            EXPECT_FALSE(read_ec);
            EXPECT_EQ(size, 0u);
            EXPECT_TRUE(body_done);
            done = true;
          });
        });
      });
    });
  ioc.run();
  server.join();
  EXPECT_TRUE(done);
}

TEST(HttpClientStreamEdgeTest, BufferedClientAlsoHandlesHeadAndInformationalResponses)
{
  net::io_context server_ioc;
  tcp::acceptor acceptor(server_ioc, {net::ip::address_v4::loopback(), 0});
  const auto port = acceptor.local_endpoint().port();
  std::thread server([&]
  {
    tcp::socket socket(server_ioc);
    acceptor.accept(socket);
    boost::beast::flat_buffer request_buffer;
    http::request<http::empty_body> request;
    http::read(socket, request_buffer, request);
    const std::string wire = "HTTP/1.1 103 Early Hints\r\n\r\n"
                             "HTTP/1.1 200 OK\r\nContent-Length: 99\r\nConnection: close\r\n\r\n";
    net::write(socket, net::buffer(wire));
  });
  net::io_context ioc;
  auto http_client = std::make_shared<client::HttpClient>(ioc);
  bool done = false;
  http_client->request(http::verb::head, "http://127.0.0.1:" + std::to_string(port) + "/",
                       {}, {}, {}, [&](boost::system::error_code ec, http::response<http::string_body> response)
  {
    EXPECT_FALSE(ec) << ec.message();
    EXPECT_EQ(response.result(), http::status::ok);
    EXPECT_EQ(response[http::field::content_length], "99");
    EXPECT_TRUE(response.body().empty());
    done = true;
  });
  ioc.run();
  server.join();
  EXPECT_TRUE(done);
}
