#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <boost/asio.hpp>

namespace arena_core::observer {

class ObserverStore;

struct ObserverHttpConfig {
  std::string bind_address{"127.0.0.1"};
  std::uint16_t port{8080};
  std::filesystem::path static_root{"web/observer"};
  bool demo_mode{false};
  std::size_t maximum_request_body_bytes{64 * 1024};
};

class ObserverHttpServer final : public std::enable_shared_from_this<ObserverHttpServer> {
 public:
  ObserverHttpServer(boost::asio::io_context& io_context,
                     ObserverHttpConfig config,
                     std::shared_ptr<ObserverStore> store);

  Error start();
  void stop();
  std::uint16_t bound_port() const;

 private:
  void do_accept();

  boost::asio::strand<boost::asio::io_context::executor_type> strand_;
  boost::asio::ip::tcp::acceptor acceptor_;
  ObserverHttpConfig config_;
  std::shared_ptr<ObserverStore> store_;
  bool started_{false};
  bool stopped_{false};
};

}  // namespace arena_core::observer
