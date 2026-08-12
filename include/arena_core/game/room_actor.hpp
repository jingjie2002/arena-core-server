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
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

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
  using TrustedResultCompletion =
      std::function<void(Result<settlement::SettlementReceipt>)>;

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
      std::chrono::milliseconds result_wait_timeout,
      PlayerNotifier notifier,
      std::shared_ptr<settlement::SettlementGateway> settlement_gateway,
      ClosedCallback closed_callback,
      std::shared_ptr<observer::ObserverStore> observer_store = nullptr);

  void async_join(std::string session_player, protocol::v1::JoinRoom command, Completion completion);
  void async_ready(std::string session_player, protocol::v1::ReadyRoom command, Completion completion);
  void async_leave(std::string session_player, protocol::v1::LeaveRoom command, Completion completion);
  void async_submit_result(std::string result_id,
                           domain::FrozenRoomResult result,
                           TrustedResultCompletion completion);
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
            std::chrono::milliseconds result_wait_timeout,
            PlayerNotifier notifier,
            std::shared_ptr<settlement::SettlementGateway> settlement_gateway,
            ClosedCallback closed_callback,
            std::shared_ptr<observer::ObserverStore> observer_store);

  void arm_result_timeout();
  void begin_settlement();
  void complete_settlement(Result<settlement::SettlementReceipt> result);
  void complete_result_waiters(const Result<settlement::SettlementReceipt>& result);
  void notify_result_committed(const settlement::SettlementReceipt& receipt);
  void notify_state();
  protocol::v1::RoomStateChanged make_state_payload() const;
  void close_once();
  bool has_member(std::string_view player_id) const noexcept;

  boost::asio::strand<boost::asio::io_context::executor_type> strand_;
  boost::asio::steady_timer result_timer_;
  domain::Room room_;
  std::string room_server_;
  std::unordered_map<std::string, JoinCredential> credentials_;
  std::chrono::milliseconds result_wait_timeout_;
  PlayerNotifier notifier_;
  std::shared_ptr<settlement::SettlementGateway> settlement_gateway_;
  ClosedCallback closed_callback_;
  std::shared_ptr<observer::ObserverStore> observer_store_;
  std::string pending_result_id_;
  std::vector<TrustedResultCompletion> result_waiters_;
  std::optional<settlement::SettlementReceipt> committed_receipt_;
  bool settlement_in_flight_{false};
  bool closed_callback_sent_{false};
  bool stopped_{false};
};

std::string digest_ephemeral_token(std::string_view token);

}  // namespace arena_core::game
