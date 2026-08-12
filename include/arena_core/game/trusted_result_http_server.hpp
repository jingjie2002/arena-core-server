#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <boost/asio.hpp>

namespace arena_core::game {

class GameService;

struct TrustedResultHttpConfig {
  std::string bind_address{"127.0.0.1"};
  std::uint16_t port{9200};
  std::string service_token;
  std::size_t maximum_request_body_bytes{16 * 1024};
};

class TrustedResultHttpServer final
    : public std::enable_shared_from_this<TrustedResultHttpServer> {
 public:
  TrustedResultHttpServer(boost::asio::io_context& io_context,
                          TrustedResultHttpConfig config,
                          std::shared_ptr<GameService> game_service);

  Error start();
  void stop();
  std::uint16_t bound_port() const;

 private:
  void do_accept();

  boost::asio::strand<boost::asio::io_context::executor_type> strand_;
  boost::asio::ip::tcp::acceptor acceptor_;
  TrustedResultHttpConfig config_;
  std::shared_ptr<GameService> game_service_;
  bool started_{false};
  bool stopped_{false};
};

}  // namespace arena_core::game
