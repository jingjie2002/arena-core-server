#include "arena_core/game/room_actor.hpp"

#include "arena_core/game/trusted_result.hpp"
#include "arena_core/observer/observer_store.hpp"
#include "arena_core/protocol/frame.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>
#include <utility>

#include <boost/uuid/detail/sha1.hpp>

namespace arena_core::game {
namespace {

namespace asio = boost::asio;
namespace wire = arena_core::protocol::v1;

wire::Envelope response_envelope(const wire::MessageType type) {
  wire::Envelope response;
  response.set_protocol_version(1);
  response.set_message_type(type);
  return response;
}

}  // namespace

std::string digest_ephemeral_token(const std::string_view token) {
  boost::uuids::detail::sha1 sha1;
  sha1.process_bytes(token.data(), token.size());
  unsigned int digest[5]{};
  sha1.get_digest(digest);
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto word : digest) {
    output << std::setw(8) << word;
  }
  return output.str();
}

Result<std::shared_ptr<RoomActor>> RoomActor::create(
    asio::io_context& io_context,
    std::string room_id,
    std::string match_id,
    std::string room_server,
    std::string first_player,
    std::string second_player,
    std::string first_join_token,
    std::string second_join_token,
    const domain::TimePoint token_deadline,
    const std::chrono::milliseconds result_wait_timeout,
    PlayerNotifier notifier,
    std::shared_ptr<settlement::SettlementGateway> settlement_gateway,
    ClosedCallback closed_callback,
    std::shared_ptr<observer::ObserverStore> observer_store) {
  auto room = domain::Room::create(std::move(room_id), std::move(match_id), first_player, second_player);
  if (!room.ok()) {
    return Result<std::shared_ptr<RoomActor>>::failure(std::move(room.error));
  }

  std::unordered_map<std::string, JoinCredential> credentials;
  credentials.emplace(std::move(first_player),
                      JoinCredential{digest_ephemeral_token(first_join_token), token_deadline});
  credentials.emplace(std::move(second_player),
                      JoinCredential{digest_ephemeral_token(second_join_token), token_deadline});
  return Result<std::shared_ptr<RoomActor>>::success(std::shared_ptr<RoomActor>(new RoomActor(
      io_context, std::move(*room.value), std::move(room_server), std::move(credentials),
      result_wait_timeout, std::move(notifier),
      std::move(settlement_gateway), std::move(closed_callback), std::move(observer_store))));
}

RoomActor::RoomActor(asio::io_context& io_context,
                     domain::Room room,
                     std::string room_server,
                     std::unordered_map<std::string, JoinCredential> credentials,
                     const std::chrono::milliseconds result_wait_timeout,
                     PlayerNotifier notifier,
                     std::shared_ptr<settlement::SettlementGateway> settlement_gateway,
                     ClosedCallback closed_callback,
                     std::shared_ptr<observer::ObserverStore> observer_store)
    : strand_(asio::make_strand(io_context)),
      result_timer_(strand_),
      room_(std::move(room)),
      room_server_(std::move(room_server)),
      credentials_(std::move(credentials)),
      result_wait_timeout_(result_wait_timeout),
      notifier_(std::move(notifier)),
      settlement_gateway_(std::move(settlement_gateway)),
      closed_callback_(std::move(closed_callback)),
      observer_store_(std::move(observer_store)) {}

void RoomActor::async_join(std::string session_player, wire::JoinRoom command, Completion completion) {
  asio::dispatch(strand_, [self = shared_from_this(), session_player = std::move(session_player),
                           command = std::move(command), completion = std::move(completion)]() mutable {
    if (self->stopped_) {
      completion(Result<wire::Envelope>::failure(make_error(ErrorCode::kServerShuttingDown)));
      return;
    }
    if (command.player_id() != session_player || command.room_id() != self->room_.room_id() ||
        command.match_id() != self->room_.match_id() || command.room_server() != self->room_server_) {
      completion(Result<wire::Envelope>::failure(make_error(ErrorCode::kRoomJoinTokenInvalid)));
      return;
    }
    const auto credential = self->credentials_.find(session_player);
    if (credential == self->credentials_.end() || domain::MonotonicClock::now() >= credential->second.deadline ||
        credential->second.digest != digest_ephemeral_token(command.join_token())) {
      completion(Result<wire::Envelope>::failure(make_error(ErrorCode::kRoomJoinTokenInvalid)));
      return;
    }
    if (auto error = self->room_.join(session_player)) {
      completion(Result<wire::Envelope>::failure(std::move(error)));
      return;
    }

    wire::RoomJoined payload;
    payload.set_room_id(self->room_.room_id());
    payload.set_state(std::string(domain::room_state_name(self->room_.state())));
    auto response = response_envelope(wire::ROOM_JOINED);
    protocol::set_payload(response, payload);
    completion(Result<wire::Envelope>::success(std::move(response)));
    self->notify_state();
  });
}

void RoomActor::async_ready(std::string session_player, wire::ReadyRoom command, Completion completion) {
  asio::dispatch(strand_, [self = shared_from_this(), session_player = std::move(session_player),
                           command = std::move(command), completion = std::move(completion)]() mutable {
    if (self->stopped_) {
      completion(Result<wire::Envelope>::failure(make_error(ErrorCode::kServerShuttingDown)));
      return;
    }
    if (command.room_id() != self->room_.room_id() || !self->has_member(session_player)) {
      completion(Result<wire::Envelope>::failure(make_error(ErrorCode::kRoomNotFound)));
      return;
    }
    if (self->room_.state() == domain::RoomState::kAwaitingResult) {
      auto payload = self->make_state_payload();
      auto response = response_envelope(wire::ROOM_STATE_CHANGED);
      protocol::set_payload(response, payload);
      completion(Result<wire::Envelope>::success(std::move(response)));
      return;
    }
    if (auto error = self->room_.set_ready(session_player)) {
      completion(Result<wire::Envelope>::failure(std::move(error)));
      return;
    }

    if (self->room_.state() == domain::RoomState::kReady) {
      auto error = self->room_.start();
      if (!error) {
        self->credentials_.clear();
      }
      if (error) {
        completion(Result<wire::Envelope>::failure(std::move(error)));
        return;
      }
      self->arm_result_timeout();
      auto payload = self->make_state_payload();
      auto response = response_envelope(wire::ROOM_STATE_CHANGED);
      protocol::set_payload(response, payload);
      completion(Result<wire::Envelope>::success(std::move(response)));
      self->notify_state();
      return;
    }

    auto payload = self->make_state_payload();
    auto response = response_envelope(wire::ROOM_STATE_CHANGED);
    protocol::set_payload(response, payload);
    completion(Result<wire::Envelope>::success(std::move(response)));
    self->notify_state();
  });
}

void RoomActor::async_submit_result(std::string result_id,
                                    domain::FrozenRoomResult result,
                                    TrustedResultCompletion completion) {
  asio::dispatch(strand_, [self = shared_from_this(), result_id = std::move(result_id),
                           result = std::move(result), completion = std::move(completion)]() mutable {
    if (self->stopped_) {
      completion(Result<settlement::SettlementReceipt>::failure(
          make_error(ErrorCode::kServerShuttingDown)));
      return;
    }
    if (result.rule_version != self->settlement_gateway_->score_rule_version()) {
      completion(Result<settlement::SettlementReceipt>::failure(
          make_error(ErrorCode::kRuleVersionMismatch)));
      return;
    }
    if (self->committed_receipt_.has_value()) {
      if (self->committed_receipt_->settlement_id != result_id ||
          self->committed_receipt_->fingerprint != result.fingerprint) {
        completion(Result<settlement::SettlementReceipt>::failure(
            make_error(ErrorCode::kSettlementConflict)));
        return;
      }
      auto receipt = *self->committed_receipt_;
      receipt.idempotent = true;
      completion(Result<settlement::SettlementReceipt>::success(std::move(receipt)));
      return;
    }
    if (self->room_.state() == domain::RoomState::kSettling) {
      if (!self->room_.result().has_value() || self->pending_result_id_ != result_id ||
          self->room_.result()->fingerprint != result.fingerprint) {
        completion(Result<settlement::SettlementReceipt>::failure(
            make_error(ErrorCode::kSettlementConflict)));
        return;
      }
      self->result_waiters_.push_back(std::move(completion));
      if (!self->settlement_in_flight_) {
        self->begin_settlement();
      }
      return;
    }
    if (auto error = self->room_.freeze_result(std::move(result))) {
      completion(Result<settlement::SettlementReceipt>::failure(std::move(error)));
      return;
    }
    self->result_timer_.cancel();
    self->pending_result_id_ = std::move(result_id);
    self->result_waiters_.push_back(std::move(completion));
    self->notify_state();
    self->begin_settlement();
  });
}

void RoomActor::arm_result_timeout() {
  result_timer_.expires_after(result_wait_timeout_);
  result_timer_.async_wait(asio::bind_executor(
      strand_, [self = shared_from_this()](const boost::system::error_code& error) {
        if (error == asio::error::operation_aborted || self->stopped_ ||
            self->room_.state() != domain::RoomState::kAwaitingResult) {
          return;
        }
        if (auto timeout_error = self->room_.expire_result_wait()) {
          static_cast<void>(timeout_error);
          return;
        }
        if (self->observer_store_) {
          self->observer_store_->publish_event("room", "room.result_timed_out",
                                               self->room_.match_id(), self->room_.room_id(),
                                               "trusted result did not arrive before deadline", "warning");
        }
        self->notify_state();
        self->close_once();
      }));
}

void RoomActor::begin_settlement() {
  if (!room_.result().has_value()) {
    complete_result_waiters(Result<settlement::SettlementReceipt>::failure(
        make_error(ErrorCode::kSettlementNotAllowed)));
    return;
  }
  if (settlement_in_flight_) {
    return;
  }
  settlement_in_flight_ = true;
  settlement::SettlementCommand command;
  command.settlement_id = pending_result_id_;
  command.match_id = room_.match_id();
  command.room_id = room_.room_id();
  command.result = *room_.result();
  settlement_gateway_->async_settle(
      strand_, std::move(command),
      [self = shared_from_this()](Result<settlement::SettlementReceipt> result) mutable {
        self->complete_settlement(std::move(result));
      });
}

void RoomActor::complete_settlement(Result<settlement::SettlementReceipt> result) {
  settlement_in_flight_ = false;
  if (!result.ok()) {
    if (observer_store_) {
      observer_store_->publish_event("settlement", "settlement.failed", room_.match_id(),
                                     room_.room_id(), result.error.message, "error");
    }
    complete_result_waiters(result);
    notify_state();
    return;
  }
  if (auto error = room_.mark_settlement_committed()) {
    complete_result_waiters(Result<settlement::SettlementReceipt>::failure(std::move(error)));
    return;
  }
  if (observer_store_ && room_.result().has_value()) {
    const auto& frozen = *room_.result();
    observer_store_->add_settlement({result.value->settlement_id, result.value->match_id,
                                     frozen.winner_id, frozen.loser_id, "Committed", room_.match_id(),
                                     result.value->idempotent});
    auto scores = result.value->scores;
    std::sort(scores.begin(), scores.end(), [](const rank::RankScore& left, const rank::RankScore& right) {
      return left.score != right.score ? left.score > right.score : left.player_id < right.player_id;
    });
    std::vector<observer::RankEntryView> rankings;
    rankings.reserve(scores.size());
    for (std::size_t index = 0; index < scores.size(); ++index) {
      rankings.push_back({scores[index].player_id, scores[index].score,
                          static_cast<std::uint32_t>(index + 1)});
    }
    observer_store_->replace_rankings("duel:season-1", std::move(rankings), result.value->cache_state);
    observer_store_->set_health("redis", result.value->cache_state == "fresh",
                                result.value->cache_state == "fresh"
                                    ? "rank cache fresh"
                                    : "rank cache stale; dirty repair pending");
    observer_store_->publish_event("settlement",
                                   result.value->idempotent ? "settlement.idempotent" : "settlement.committed",
                                   room_.match_id(), result.value->settlement_id,
                                   "MySQL result committed before room close", "success");
  }
  committed_receipt_ = *result.value;
  notify_result_committed(*result.value);
  complete_result_waiters(result);
  notify_state();
  close_once();
}

void RoomActor::complete_result_waiters(const Result<settlement::SettlementReceipt>& result) {
  auto waiters = std::move(result_waiters_);
  result_waiters_.clear();
  for (auto& waiter : waiters) {
    if (result.ok()) {
      waiter(Result<settlement::SettlementReceipt>::success(*result.value));
    } else {
      waiter(Result<settlement::SettlementReceipt>::failure(result.error));
    }
  }
}

void RoomActor::notify_result_committed(const settlement::SettlementReceipt& receipt) {
  if (!notifier_) {
    return;
  }
  const auto payload = make_match_result_view(receipt);
  for (const auto& member : room_.members()) {
    auto event = response_envelope(wire::MATCH_RESULT_COMMITTED);
    protocol::set_payload(event, payload);
    notifier_(member.player_id, std::move(event));
  }
}

void RoomActor::async_leave(std::string session_player, wire::LeaveRoom command, Completion completion) {
  asio::dispatch(strand_, [self = shared_from_this(), session_player = std::move(session_player),
                           command = std::move(command), completion = std::move(completion)]() mutable {
    if (command.room_id() != self->room_.room_id() || !self->has_member(session_player)) {
      completion(Result<wire::Envelope>::failure(make_error(ErrorCode::kRoomNotFound)));
      return;
    }
    if (auto error = self->room_.abort()) {
      completion(Result<wire::Envelope>::failure(std::move(error)));
      return;
    }
    wire::RoomLeft payload;
    payload.set_room_id(self->room_.room_id());
    payload.set_state(std::string(domain::room_state_name(self->room_.state())));
    auto response = response_envelope(wire::ROOM_LEFT);
    protocol::set_payload(response, payload);
    completion(Result<wire::Envelope>::success(std::move(response)));
    self->notify_state();
    self->close_once();
  });
}

void RoomActor::stop() {
  asio::dispatch(strand_, [self = shared_from_this()] {
    self->stopped_ = true;
    self->result_timer_.cancel();
    self->complete_result_waiters(Result<settlement::SettlementReceipt>::failure(
        make_error(ErrorCode::kServerShuttingDown)));
  });
}

wire::RoomStateChanged RoomActor::make_state_payload() const {
  wire::RoomStateChanged payload;
  payload.set_room_id(room_.room_id());
  payload.set_state(std::string(domain::room_state_name(room_.state())));
  for (const auto& member : room_.members()) {
    if (member.joined) {
      payload.add_joined_players(member.player_id);
    }
    if (member.ready) {
      payload.add_ready_players(member.player_id);
    }
  }
  if (room_.result().has_value()) {
    payload.set_winner_id(room_.result()->winner_id);
    payload.set_loser_id(room_.result()->loser_id);
  }
  return payload;
}

void RoomActor::notify_state() {
  if (observer_store_) {
    observer::RoomView view;
    view.room_id = room_.room_id();
    view.match_id = room_.match_id();
    view.state = std::string(domain::room_state_name(room_.state()));
    view.trace_id = room_.match_id();
    for (const auto& member : room_.members()) {
      view.players.push_back(member.player_id);
      if (member.ready) {
        view.ready_players.push_back(member.player_id);
      }
    }
    if (room_.result().has_value()) {
      view.winner_id = room_.result()->winner_id;
    }
    observer_store_->upsert_room(std::move(view));
    observer_store_->publish_event("room", "room.state_changed", room_.match_id(), room_.room_id(),
                                   std::string(domain::room_state_name(room_.state())));
  }
  if (!notifier_) {
    return;
  }
  const auto payload = make_state_payload();
  for (const auto& member : room_.members()) {
    auto event = response_envelope(wire::ROOM_STATE_CHANGED);
    protocol::set_payload(event, payload);
    notifier_(member.player_id, std::move(event));
  }
}

void RoomActor::close_once() {
  if (closed_callback_sent_) {
    return;
  }
  closed_callback_sent_ = true;
  if (closed_callback_) {
    closed_callback_(room_.room_id());
  }
}

bool RoomActor::has_member(const std::string_view player_id) const noexcept {
  return std::any_of(room_.members().begin(), room_.members().end(), [player_id](const domain::RoomMember& member) {
    return member.player_id == player_id;
  });
}

}  // namespace arena_core::game
