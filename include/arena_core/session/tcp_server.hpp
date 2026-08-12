#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/session/business_service.hpp"
#include "arena_core/session/session.hpp"
#include "arena_core/session/session_registry.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>

#include <boost/asio.hpp>

namespace arena_core::observer {
class ObserverStore;
}

namespace arena_core::session {

class TcpServer final : public std::enable_shared_from_this<TcpServer> {
 public:
  TcpServer(boost::asio::io_context& io_context,
            boost::asio::ip::tcp::endpoint endpoint,
            SessionOptions session_options,
            std::shared_ptr<TokenGenerator> token_generator,
            std::shared_ptr<BusinessService> business_service = {},
            std::shared_ptr<observer::ObserverStore> observer_store = {});

  Error start();
  void stop();
  std::uint16_t bound_port() const;
  std::size_t active_connection_count() const noexcept { return active_connection_count_.load(); }
  std::size_t reconnectable_session_count() const { return registry_.size(); }

 private:
  void do_accept();
  void remove_session(std::uint64_t session_id);
  void arm_registry_sweep();
  void push_to_player(std::string_view player_id, protocol::v1::Envelope envelope);

  boost::asio::strand<boost::asio::io_context::executor_type> strand_;
  boost::asio::ip::tcp::acceptor acceptor_;
  boost::asio::ip::tcp::endpoint endpoint_;
  boost::asio::steady_timer sweep_timer_;
  SessionOptions session_options_;
  SessionRegistry registry_;
  std::shared_ptr<BusinessService> business_service_;
  std::shared_ptr<observer::ObserverStore> observer_store_;
  std::unordered_map<std::uint64_t, std::shared_ptr<Session>> sessions_;
  std::atomic<std::size_t> active_connection_count_{0};
  std::uint64_t next_session_id_{0};
  bool started_{false};
  bool stopped_{false};
};

}  // namespace arena_core::session
