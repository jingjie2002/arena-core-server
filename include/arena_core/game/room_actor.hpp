#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/core/result.hpp"
#include "arena_core/domain/clock.hpp"
#include "arena_core/domain/room.hpp"
#include "arena_core/session/business_service.hpp"
#include "arena_core/settlement/settlement_gateway.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include <boost/asio.hpp>

#include "arena_core.pb.h"

namespace arena_core::observer {
class ObserverStore;
}

namespace arena_core::game {

class RoomActor final : public std::enable_shared_from_this<RoomActor> {
 public:
  using Completion = session::BusinessService::Completion;
  using PlayerNotifier = session::BusinessService::PlayerNotifier;
  using ClosedCallback = std::function<void(std::string)>;

  static Result<std::shared_ptr<RoomActor>> create(
      boost::asio::io_context& io_context,
      std::string room_id,
      std::string match_id,
      std::string room_server,
      std::string first_player,
      std::string second_player,
      std::string first_join_token,
      std::string second_join_token,
      domain::TimePoint token_deadline,
      PlayerNotifier notifier,
      std::shared_ptr<settlement::SettlementGateway> settlement_gateway,
      ClosedCallback closed_callback,
      std::shared_ptr<observer::ObserverStore> observer_store = nullptr);

  void async_join(std::string session_player, protocol::v1::JoinRoom command, Completion completion);
  void async_ready(std::string session_player, protocol::v1::ReadyRoom command, Completion completion);
  void async_leave(std::string session_player, protocol::v1::LeaveRoom command, Completion completion);
  void stop();

  const std::string& room_id() const noexcept { return room_.room_id(); }
  const std::string& match_id() const noexcept { return room_.match_id(); }

 private:
  struct JoinCredential {
    std::string digest;
    domain::TimePoint deadline;
  };

  RoomActor(boost::asio::io_context& io_context,
            domain::Room room,
            std::string room_server,
            std::unordered_map<std::string, JoinCredential> credentials,
            PlayerNotifier notifier,
            std::shared_ptr<settlement::SettlementGateway> settlement_gateway,
            ClosedCallback closed_callback,
            std::shared_ptr<observer::ObserverStore> observer_store);

  void begin_settlement(Completion completion);
  void complete_settlement(Completion completion, Result<settlement::SettlementReceipt> result);
  void notify_state();
  protocol::v1::RoomStateChanged make_state_payload() const;
  void close_once();
  bool has_member(std::string_view player_id) const noexcept;

  boost::asio::strand<boost::asio::io_context::executor_type> strand_;
  domain::Room room_;
  std::string room_server_;
  std::unordered_map<std::string, JoinCredential> credentials_;
  PlayerNotifier notifier_;
  std::shared_ptr<settlement::SettlementGateway> settlement_gateway_;
  ClosedCallback closed_callback_;
  std::shared_ptr<observer::ObserverStore> observer_store_;
  bool settlement_in_flight_{false};
  bool closed_callback_sent_{false};
  bool stopped_{false};
};

std::string digest_ephemeral_token(std::string_view token);

}  // namespace arena_core::game
