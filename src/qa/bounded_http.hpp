#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

namespace arena_core::qa::detail {

namespace bounded_http_asio = boost::asio;
namespace bounded_http_beast = boost::beast;
namespace bounded_http_http = boost::beast::http;
using BoundedHttpTcp = bounded_http_asio::ip::tcp;

struct BoundedHttpResult {
  std::optional<bounded_http_http::response<bounded_http_http::string_body>> response;
  std::string error;
};

class BoundedHttpExchange final
    : public std::enable_shared_from_this<BoundedHttpExchange> {
 public:
  BoundedHttpExchange(
      bounded_http_asio::io_context& io_context,
      std::string host,
      std::string port,
      bounded_http_http::request<bounded_http_http::string_body> request,
      const std::chrono::milliseconds timeout,
      const std::uint64_t maximum_response_bytes)
      : resolver_(io_context), stream_(io_context), timer_(io_context),
        host_(std::move(host)), port_(std::move(port)),
        request_(std::move(request)), timeout_(timeout) {
    parser_.body_limit(maximum_response_bytes);
  }

  void start() {
    timer_.expires_after(timeout_);
    timer_.async_wait([self = shared_from_this()](const boost::system::error_code& error) {
      if (!error) self->finish(bounded_http_asio::error::timed_out);
    });
    resolver_.async_resolve(
        host_, port_,
        [self = shared_from_this()](const boost::system::error_code& error,
                                    BoundedHttpTcp::resolver::results_type endpoints) {
          if (error || self->completed_) {
            if (error) self->finish(error);
            return;
          }
          self->stream_.async_connect(
              endpoints,
              [self](const boost::system::error_code& connect_error,
                     const BoundedHttpTcp::endpoint&) {
                if (connect_error || self->completed_) {
                  if (connect_error) self->finish(connect_error);
                  return;
                }
                bounded_http_http::async_write(
                    self->stream_, self->request_,
                    [self](const boost::system::error_code& write_error, std::size_t) {
                      if (write_error || self->completed_) {
                        if (write_error) self->finish(write_error);
                        return;
                      }
                      bounded_http_http::async_read(
                          self->stream_, self->buffer_, self->parser_,
                          [self](const boost::system::error_code& read_error, std::size_t) {
                            self->finish(read_error);
                          });
                    });
              });
        });
  }

  BoundedHttpResult result() {
    BoundedHttpResult result;
    if (!error_ && parser_.is_done()) result.response.emplace(parser_.release());
    else result.error = error_ ? error_.message() : "HTTP exchange did not complete";
    return result;
  }

 private:
  void finish(const boost::system::error_code& error) {
    if (completed_) return;
    completed_ = true;
    error_ = error;
    boost::system::error_code ignored;
    // Boost 1.74 exposes only the throwing overload for steady_timer::cancel.
    // This callback runs on the timer's own io_context, so cancellation is
    // expected to succeed; keep the exchange completion noexcept regardless.
    try {
      static_cast<void>(timer_.cancel());
    } catch (const boost::system::system_error&) {
    }
    resolver_.cancel();
    stream_.socket().cancel(ignored);
    stream_.socket().shutdown(BoundedHttpTcp::socket::shutdown_both, ignored);
    stream_.socket().close(ignored);
  }

  BoundedHttpTcp::resolver resolver_;
  bounded_http_beast::tcp_stream stream_;
  bounded_http_asio::steady_timer timer_;
  std::string host_;
  std::string port_;
  bounded_http_http::request<bounded_http_http::string_body> request_;
  bounded_http_beast::flat_buffer buffer_;
  bounded_http_http::response_parser<bounded_http_http::string_body> parser_;
  std::chrono::milliseconds timeout_;
  boost::system::error_code error_;
  bool completed_{false};
};

inline BoundedHttpResult bounded_http_request(
    const std::string& host,
    const std::uint16_t port,
    bounded_http_http::request<bounded_http_http::string_body> request,
    const std::chrono::milliseconds timeout,
    const std::uint64_t maximum_response_bytes = 64U * 1'024U) {
  bounded_http_asio::io_context io_context;
  auto exchange = std::make_shared<BoundedHttpExchange>(
      io_context, host, std::to_string(port), std::move(request), timeout,
      maximum_response_bytes);
  exchange->start();
  io_context.run();
  return exchange->result();
}

}  // namespace arena_core::qa::detail
