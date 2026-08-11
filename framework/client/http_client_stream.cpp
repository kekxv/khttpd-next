#include "http_client_stream.hpp"

#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/url.hpp>
#include <limits>
#include <optional>
#include "io_context_pool.hpp"

namespace khttpd::framework::client
{
  namespace beast = boost::beast;
  namespace http = beast::http;
  namespace net = boost::asio;
  namespace ssl = net::ssl;
  using tcp = net::ip::tcp;

  namespace
  {
    std::shared_ptr<ssl::context> default_ssl_context()
    {
      auto context = std::make_shared<ssl::context>(ssl::context::tls_client);
      context->set_default_verify_paths();
      context->set_verify_mode(ssl::verify_peer);
      return context;
    }

    std::shared_ptr<ssl::context> borrowed_ssl_context(ssl::context& context)
    {
      return {&context, [](ssl::context*) {}};
    }
  }

  struct HttpClientStream::Impl : std::enable_shared_from_this<HttpClientStream::Impl>
  {
    using TlsStream = beast::ssl_stream<beast::tcp_stream>;

    net::any_io_executor executor;
    tcp::resolver resolver;
    std::shared_ptr<ssl::context> ssl_context;
    std::unique_ptr<beast::tcp_stream> plain_stream;
    std::unique_ptr<TlsStream> tls_stream;
    beast::flat_buffer read_buffer;
    http::request<http::buffer_body> request;
    std::optional<http::request_serializer<http::buffer_body>> request_serializer;
    std::optional<http::response_parser<http::buffer_body>> response_parser;
    http::verb request_method = http::verb::unknown;
    std::string host;
    std::string port;
    bool use_tls = false;
    bool started = false;
    bool request_finished = false;

    Impl(net::io_context& ioc, std::shared_ptr<ssl::context> context)
      : executor(net::make_strand(ioc)), resolver(executor), ssl_context(std::move(context))
    {
    }

    void start(const std::string& url, RequestHead head, Callback callback)
    {
      net::post(executor, [self = shared_from_this(), url, head = std::move(head),
                           callback = std::move(callback)]() mutable
      { self->start_on_executor(url, std::move(head), std::move(callback)); });
    }

    void start_on_executor(const std::string& url, RequestHead head, Callback callback)
    {
      const auto parsed = boost::urls::parse_uri(url);
      if (!parsed || (parsed->scheme() != "http" && parsed->scheme() != "https"))
        return callback(make_error_code(boost::system::errc::operation_not_supported));

      use_tls = parsed->scheme() == "https";
      host = parsed->host();
      port = parsed->port().empty() ? (use_tls ? "443" : "80") : std::string(parsed->port());
      std::string target(parsed->encoded_target());
      if (target.empty()) target = "/";
      else if (target.front() == '?') target.insert(target.begin(), '/');
      request.method(head.method());
      request_method = head.method();
      request.target(target);
      request.version(head.version());
      request.keep_alive(head.keep_alive());
      for (const auto& field : head) request.insert(field.name_string(), field.value());
      const bool default_port = (use_tls && port == "443") || (!use_tls && port == "80");
      request.set(http::field::host, default_port ? host : host + ":" + port);
      request.body().more = true;
      request_serializer.emplace(request);

      if (use_tls)
      {
        tls_stream = std::make_unique<TlsStream>(executor, *ssl_context);
        if (!SSL_set_tlsext_host_name(tls_stream->native_handle(), host.c_str()))
          return callback(beast::error_code(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()));
        tls_stream->set_verify_callback(ssl::host_name_verification(host));
      }
      else
      {
        plain_stream = std::make_unique<beast::tcp_stream>(executor);
      }

      resolver.async_resolve(host, port, [self = shared_from_this(), callback = std::move(callback)]
        (beast::error_code ec, tcp::resolver::results_type results) mutable
      {
        if (ec) return callback(ec);
        self->async_connect(std::move(results), std::move(callback));
      });
    }

    void async_connect(tcp::resolver::results_type results, Callback callback)
    {
      if (use_tls)
      {
        beast::get_lowest_layer(*tls_stream).async_connect(results,
          [self = shared_from_this(), callback = std::move(callback)]
          (beast::error_code ec, const tcp::endpoint&) mutable
          {
            if (ec) return callback(ec);
            self->tls_stream->async_handshake(ssl::stream_base::client,
              [self, callback = std::move(callback)](beast::error_code handshake_ec) mutable
              {
                if (handshake_ec) return callback(handshake_ec);
                self->async_write_header(*self->tls_stream, std::move(callback));
              });
          });
      }
      else
      {
        plain_stream->async_connect(results,
          [self = shared_from_this(), callback = std::move(callback)]
          (beast::error_code ec, const tcp::endpoint&) mutable
          {
            if (ec) return callback(ec);
            self->async_write_header(*self->plain_stream, std::move(callback));
          });
      }
    }

    template <class Stream>
    void async_write_header(Stream& stream, Callback callback)
    {
      http::async_write_header(stream, *request_serializer,
        [self = shared_from_this(), callback = std::move(callback)]
        (beast::error_code ec, std::size_t) mutable
        {
          self->started = !ec;
          callback(ec);
        });
    }

    void write(net::const_buffer source, Callback callback)
    {
      net::post(executor, [self = shared_from_this(), source, callback = std::move(callback)]() mutable
      { self->write_on_executor(source, std::move(callback)); });
    }

    void write_on_executor(net::const_buffer source, Callback callback)
    {
      if (!started || request_finished) return callback(net::error::operation_aborted);
      request.body().data = const_cast<void*>(source.data());
      request.body().size = source.size();
      request.body().more = true;
      if (use_tls) async_write_body(*tls_stream, std::move(callback));
      else async_write_body(*plain_stream, std::move(callback));
    }

    template <class Stream>
    void async_write_body(Stream& stream, Callback callback)
    {
      http::async_write(stream, *request_serializer,
        [callback = std::move(callback)](beast::error_code ec, std::size_t) mutable
        { if (ec == http::error::need_buffer) ec = {}; callback(ec); });
    }

    void finish(Callback callback)
    {
      net::post(executor, [self = shared_from_this(), callback = std::move(callback)]() mutable
      { self->finish_on_executor(std::move(callback)); });
    }

    void finish_on_executor(Callback callback)
    {
      if (!started || request_finished) return callback(net::error::operation_aborted);
      request_finished = true;
      if (request_serializer->is_done()) return callback({});
      request.body().data = nullptr;
      request.body().size = 0;
      request.body().more = false;
      if (use_tls) async_write_body(*tls_stream, std::move(callback));
      else async_write_body(*plain_stream, std::move(callback));
    }

    void read_head(ResponseHeadCallback callback)
    {
      net::post(executor, [self = shared_from_this(), callback = std::move(callback)]() mutable
      { self->read_head_on_executor(std::move(callback)); });
    }

    void read_head_on_executor(ResponseHeadCallback callback)
    {
      if (!started) return callback(net::error::operation_aborted, {});
      if (use_tls) async_read_head(*tls_stream, std::move(callback));
      else async_read_head(*plain_stream, std::move(callback));
    }

    template <class Stream>
    void async_read_head(Stream& stream, ResponseHeadCallback callback)
    {
      response_parser.emplace();
      response_parser->body_limit((std::numeric_limits<std::uint64_t>::max)());
      response_parser->skip(request_method == http::verb::head);
      http::async_read_header(stream, read_buffer, *response_parser,
        [self = shared_from_this(), callback = std::move(callback)](beast::error_code ec, std::size_t) mutable
        {
          ResponseHead head;
          if (!ec)
          {
            const auto& source = self->response_parser->get();
            head.result(source.result());
            head.version(source.version());
            head.keep_alive(source.keep_alive());
            for (const auto& field : source) head.insert(field.name_string(), field.value());
            if (head.result_int() >= 100 && head.result_int() < 200 &&
                head.result() != http::status::switching_protocols)
            {
              if (self->use_tls) return self->async_read_head(*self->tls_stream, std::move(callback));
              return self->async_read_head(*self->plain_stream, std::move(callback));
            }
          }
          callback(ec, std::move(head));
        });
    }

    void read(net::mutable_buffer target, ReadCallback callback)
    {
      net::post(executor, [self = shared_from_this(), target, callback = std::move(callback)]() mutable
      { self->read_on_executor(target, std::move(callback)); });
    }

    void read_on_executor(net::mutable_buffer target, ReadCallback callback)
    {
      if (!started) return callback(net::error::operation_aborted, 0, true);
      if (!response_parser) return callback(net::error::operation_aborted, 0, true);
      if (response_parser->is_done()) return callback({}, 0, true);
      auto& body = response_parser->get().body();
      body.data = target.data();
      body.size = target.size();
      if (use_tls) async_read_body(*tls_stream, target.size(), std::move(callback));
      else async_read_body(*plain_stream, target.size(), std::move(callback));
    }

    template <class Stream>
    void async_read_body(Stream& stream, std::size_t capacity, ReadCallback callback)
    {
      http::async_read_some(stream, read_buffer, *response_parser,
        [self = shared_from_this(), capacity, callback = std::move(callback)]
        (beast::error_code ec, std::size_t) mutable
        {
          if (ec == http::error::need_buffer) ec = {};
          const auto produced = capacity - self->response_parser->get().body().size;
          callback(ec, produced, self->response_parser->is_done());
        });
    }

    void cancel()
    {
      beast::error_code ignored;
      resolver.cancel();
      if (tls_stream)
      {
        beast::get_lowest_layer(*tls_stream).cancel();
        beast::get_lowest_layer(*tls_stream).socket().close(ignored);
      }
      if (plain_stream)
      {
        plain_stream->cancel();
        plain_stream->socket().close(ignored);
      }
    }
  };

  HttpClientStream::HttpClientStream()
    : impl_(std::make_shared<Impl>(IoContextPool::instance().get_io_context(), default_ssl_context())) {}
  HttpClientStream::HttpClientStream(ssl::context& context)
    : impl_(std::make_shared<Impl>(IoContextPool::instance().get_io_context(), borrowed_ssl_context(context))) {}
  HttpClientStream::HttpClientStream(net::io_context& ioc)
    : impl_(std::make_shared<Impl>(ioc, default_ssl_context())) {}
  HttpClientStream::HttpClientStream(net::io_context& ioc, ssl::context& context)
    : impl_(std::make_shared<Impl>(ioc, borrowed_ssl_context(context))) {}
  HttpClientStream::~HttpClientStream() { if (impl_) impl_->cancel(); }
  void HttpClientStream::async_start(const std::string& url, RequestHead head, Callback cb)
  { impl_->start(url, std::move(head), std::move(cb)); }
  void HttpClientStream::async_write_some(net::const_buffer buffer, Callback cb)
  { impl_->write(buffer, std::move(cb)); }
  void HttpClientStream::async_finish_request(Callback cb) { impl_->finish(std::move(cb)); }
  void HttpClientStream::async_read_response_head(ResponseHeadCallback cb) { impl_->read_head(std::move(cb)); }
  void HttpClientStream::async_read_some(net::mutable_buffer buffer, ReadCallback cb)
  { impl_->read(buffer, std::move(cb)); }
  void HttpClientStream::cancel() { impl_->cancel(); }
}
