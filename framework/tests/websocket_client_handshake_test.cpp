#include "framework/client/websocket_client.hpp"

#include <gtest/gtest.h>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <thread>

namespace fw = khttpd::framework;
namespace client = khttpd::framework::client;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

TEST(WebsocketClientHandshakeTest, PreservesEncodedTargetAndNegotiatesSubprotocol)
{
  net::io_context server_ioc;
  tcp::acceptor acceptor(server_ioc, {net::ip::address_v4::loopback(), 0});
  const auto port = acceptor.local_endpoint().port();
  std::string received_target;
  std::string requested_protocols;
  std::thread server([&]
  {
    tcp::socket socket(server_ioc);
    acceptor.accept(socket);
    beast::flat_buffer request_buffer;
    http::request<http::string_body> request;
    http::read(socket, request_buffer, request);
    received_target = std::string(request.target());
    requested_protocols = std::string(request[http::field::sec_websocket_protocol]);

    websocket::stream<tcp::socket> ws(std::move(socket));
    ws.set_option(websocket::stream_base::decorator([](websocket::response_type& response)
    {
      response.set(http::field::sec_websocket_protocol, "chat.v2");
    }));
    ws.accept(request);
    beast::error_code ignored;
    ws.next_layer().shutdown(tcp::socket::shutdown_both, ignored);
    ws.next_layer().close(ignored);
  });

  net::io_context client_ioc;
  auto ws = std::make_shared<client::WebsocketClient>(client_ioc);
  ws->set_subprotocols({"chat.v1", "chat.v2"});
  bool connected = false;
  std::string selected;
  ws->connect("ws://127.0.0.1:" + std::to_string(port) +
              "/socket%2Fv1?tenant=acme&trace=a%2Fb",
    [&](beast::error_code ec)
    {
      ASSERT_FALSE(ec) << ec.message();
      connected = true;
      selected = ws->negotiated_subprotocol();
    });
  client_ioc.run();
  server.join();

  EXPECT_TRUE(connected);
  EXPECT_EQ(received_target, "/socket%2Fv1?tenant=acme&trace=a%2Fb");
  EXPECT_EQ(requested_protocols, "chat.v1, chat.v2");
  EXPECT_EQ(selected, "chat.v2");
}
