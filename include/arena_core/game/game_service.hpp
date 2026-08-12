#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/domain/identifiers.hpp"
#include "arena_core/domain/match_ticket.hpp"
#include "arena_core/game/room_actor.hpp"
#include "arena_core/session/business_service.hpp"
#include "arena_core/session/token_generator.hpp"
#include "arena_core/settlement/settlement_gateway.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/asio.hpp>

namespace arena_core::observer {
class ObserverStore;
}

namespace arena_core::game {

struct GameServiceOptions {
  std::size_t maximum_active_rooms{1'024};
  std::chrono::milliseconds match_tick{100};
  std::chrono::milliseconds join_token_ttl{std::chrono::seconds(30)};
  std::string room_server{"arena-core-local"};
};

class GameService final : public session::BusinessService,
                          public std::enable_shared_from_this<GameService> {
 public:
  GameService(boost::asio::io_context& io_context,
              GameServiceOptions options,
              std::shared_ptr<domain::IdGenerator> id_generator,
              std::shared_ptr<session::TokenGenerator> token_generator,
              std::shared_ptr<settlement::SettlementGateway> settlement_gateway = nullptr,
              std::shared_ptr<observer::ObserverStore> observer_store = nullptr);

  void start() override;
  void stop() override;
  void set_player_notifier(PlayerNotifier notifier) override;
  void handle(std::string player_id, protocol::v1::Envelope request, Completion completion) override;

  std::size_t queued_ticket_count() const noexcept { return queued_ticket_count_.load(); }
  std::size_t active_room_count() const noexcept { return active_room_count_.load(); }

 private:
  void handle_queue(std::string player_id, protocol::v1::Envelope request, Completion completion);
  void handle_cancel(std::string player_id, protocol::v1::Envelope request, Completion completion);
  void handle_join(std::string player_id, protocol::v1::Envelope request, Completion completion);
  void handle_ready(std::string player_id, protocol::v1::Envelope request, Completion completion);
  void handle_leave(std::string player_id, protocol::v1::Envelope request, Completion completion);
  void handle_rank(std::string player_id, protocol::v1::Envelope request, Completion completion);
  void sweep_and_match();
  bool try_match_once(domain::TimePoint now);
  Result<std::int32_t> allowed_mmr_delta(const domain::MatchTicket& ticket, domain::TimePoint now) const;
  void remove_from_queue(const std::string& first_ticket, const std::string& second_ticket);
  void notify_expired(const domain::MatchTicket& ticket);
  void notify_match_found(const domain::MatchTicket& ticket,
                          const domain::MatchTicket& opponent,
                          const std::string& match_id,
                          const std::string& room_id,
                          const std::string& join_token);
  void room_closed(std::string room_id);
  void arm_tick();

  boost::asio::io_context& io_context_;
  boost::asio::strand<boost::asio::io_context::executor_type> strand_;
  boost::asio::steady_timer tick_timer_;
  GameServiceOptions options_;
  std::shared_ptr<domain::IdGenerator> id_generator_;
  std::shared_ptr<session::TokenGenerator> token_generator_;
  std::shared_ptr<settlement::SettlementGateway> settlement_gateway_;
  std::shared_ptr<observer::ObserverStore> observer_store_;
  PlayerNotifier notifier_;

  std::unordered_map<std::string, domain::MatchTicket> tickets_;
  std::unordered_map<std::string, std::string> active_ticket_by_player_;
  std::vector<std::string> queue_;
  std::unordered_map<std::string, std::shared_ptr<RoomActor>> rooms_;
  std::atomic<std::size_t> queued_ticket_count_{0};
  std::atomic<std::size_t> active_room_count_{0};
  bool started_{false};
  bool stopped_{false};
};

}  // namespace arena_core::game
