// framework/websocket/websocket_session.cpp
#include "websocket_session.hpp"
#include "context/websocket_context.hpp"
#include <fmt/core.h>
#include <boost/uuid/uuid_io.hpp>

namespace khttpd::framework
{
  std::mutex WebsocketSession::m_sessions_mutex{};
  std::map<std::string, std::shared_ptr<WebsocketSession>> WebsocketSession::m_sessions_id_{};
  std::mutex WebsocketSession::m_gen_mutex{};
  boost::uuids::random_generator WebsocketSession::gen{};

  WebsocketSession::WebsocketSession(tcp::socket&& socket, WebsocketRouter& ws_router,
                                     const std::string& initial_path)
    : ws_(std::move(socket)),
      websocket_router_(ws_router),
      initial_path_(initial_path)
  {
    {
      std::unique_lock<std::mutex> lock(m_gen_mutex);
      id = boost::uuids::to_string(gen());
    }
    ws_.read_message_max(32 * 1024 * 1024);
    ws_.set_option(ws::stream_base::decorator([](ws::response_type& res)
    {
      res.set(http::field::server, std::string(BOOST_BEAST_VERSION_STRING) + " khttpd-websocket");
    }));
  }


  void WebsocketSession::on_handshake(beast::error_code ec)
  {
    if (ec)
    {
      fmt::print(stderr, "WebSocket handshake error for path '{}': {}\n", initial_path_, ec.message());
      do_close(ec);
      return;
    }
    fmt::print("WebSocket handshake successful for path: {}\n", initial_path_);

    WebsocketContext open_ctx(shared_from_this(), initial_path_);
    {
      std::unique_lock<std::mutex> lock{m_sessions_mutex};
      m_sessions_id_[id] = shared_from_this();
    }
    websocket_router_.dispatch_open(initial_path_, open_ctx);

    do_read();
  }

  void WebsocketSession::do_read()
  {
    ws_.async_read(buffer_,
                   beast::bind_front_handler(&WebsocketSession::on_read, shared_from_this()));
  }

  void WebsocketSession::on_read(beast::error_code ec, std::size_t bytes_transferred)
  {
    boost::ignore_unused(bytes_transferred);

    if (ec == ws::error::closed)
    {
      fmt::print("WebSocket connection for path '{}' closed by client.\n", initial_path_);
      do_close(ec);
      return;
    }
    if (ec)
    {
      fmt::print(stderr, "WebSocket read error for path '{}': {}\n", initial_path_, ec.message());
      do_close(ec);
      return;
    }

    std::string received_message = beast::buffers_to_string(buffer_.data());
    bool is_text = ws_.got_text();

    fmt::print("Received WS message on path '{}': {}\n", initial_path_, received_message);

    buffer_.consume(buffer_.size());

    WebsocketContext message_ctx(shared_from_this(), received_message, is_text, initial_path_);
    websocket_router_.dispatch_message(initial_path_, message_ctx);

    do_read();
  }

  void WebsocketSession::send_message(const std::string& msg, bool is_text_msg)
  {
    auto ss = std::make_shared<const std::string>(msg);
    write_queue_.emplace(ss, is_text_msg);
    if (!writing_)
    {
      writing_ = true;
      do_write_next();
    }
  }

  void WebsocketSession::do_write_next()
  {
    if (write_queue_.empty())
    {
      writing_ = false;
      return;
    }

    auto item = std::move(write_queue_.front());
    write_queue_.pop();
    auto& ss = item.first;
    auto is_text_msg = item.second;

    ws_.text(is_text_msg);

    if (ss->length() < auto_fragment_threshold_)
    {
      ws_.async_write(net::buffer(*ss),
                      beast::bind_front_handler(&WebsocketSession::on_write, shared_from_this()));
    }
    else
    {
      auto buffer_sequence_ptr = std::make_shared<std::vector<net::const_buffer>>();
      buffer_sequence_ptr->reserve(ss->length() / fragment_size_ + 1);

      size_t offset = 0;
      while (offset < ss->length())
      {
        size_t current_chunk_size = std::min(fragment_size_, ss->length() - offset);
        buffer_sequence_ptr->emplace_back(ss->data() + offset, current_chunk_size);
        offset += current_chunk_size;
      }

      ws_.async_write(
        *buffer_sequence_ptr,
        [ss, buffer_sequence_ptr, self = shared_from_this()](beast::error_code ec, std::size_t bytes)
        {
          self->on_write(ec, bytes);
        }
      );
    }
  }

  bool WebsocketSession::send_message(const std::string& id, const std::string& msg, bool is_text)
  {
    return send_message(std::vector<std::string>{id}, msg, is_text) > 0;
  }

  size_t WebsocketSession::send_message(const std::vector<std::string>& ids, const std::string& msg, bool is_text)
  {
    // Collect target session pointers under lock, then release before sending
    std::vector<std::shared_ptr<WebsocketSession>> targets;
    {
      std::unique_lock<std::mutex> lock{m_sessions_mutex};
      for (const auto& id : ids)
      {
        auto item = m_sessions_id_.find(id);
        if (item == m_sessions_id_.end())
        {
          continue;
        }
        targets.push_back(item->second);
      }
    }
    // Send messages outside the lock
    size_t count = 0;
    for (const auto& session : targets)
    {
      session->send_message(msg, is_text);
      count++;
    }
    return count;
  }

  void WebsocketSession::on_write(beast::error_code ec, std::size_t bytes_transferred)
  {
    boost::ignore_unused(bytes_transferred);

    if (ec)
    {
      fmt::print(stderr, "WebSocket write error for path '{}': {}\n", initial_path_, ec.message());
      do_close(ec);
      return;
    }
  }

  void WebsocketSession::do_close(beast::error_code ec)
  {
    if (ec && ec != ws::error::closed && ec != boost::asio::error::eof)
    {
      WebsocketContext error_ctx(shared_from_this(), initial_path_, ec);
      websocket_router_.dispatch_error(initial_path_, error_ctx);
    }
    else
    {
      WebsocketContext close_ctx(shared_from_this(), initial_path_, ec);
      {
        std::unique_lock<std::mutex> lock{m_sessions_mutex};
        m_sessions_id_.erase(id);
      }
      websocket_router_.dispatch_close(initial_path_, close_ctx);
    }
    // Close the WebSocket stream to properly release the TCP connection
    beast::error_code close_ec;
    ws_.close(ws::close_code::normal, close_ec);
    if (close_ec && close_ec != boost::asio::error::operation_aborted)
    {
      fmt::print(stderr, "WebSocket close error for path '{}': {}\n", initial_path_, close_ec.message());
    }
  }
}
