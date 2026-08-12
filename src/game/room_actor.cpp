#include "arena_core/game/room_actor.hpp"

#include "arena_core/observer/observer_store.hpp"
#include "arena_core/protocol/frame.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
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

std::uint64_t deterministic_seed(const std::string_view value) noexcept {
  std::uint64_t hash = 14'695'981'039'346'656'037ULL;
  for (const unsigned char byte : value) {
    hash ^= byte;
    hash *= 1'099'511'628'211ULL;
  }
  return hash;
}

domain::FrozenRoomResult generate_result(const domain::Room& room, const std::string& rule_version) {
  const auto canonical = room.match_id() + "|" + room.members()[0].player_id + "|" + room.members()[1].player_id +
                         "|" + rule_version;
  const auto seed = deterministic_seed(canonical);
  const auto winner_index = static_cast<std::size_t>(seed % 2U);
  const auto loser_index = 1U - winner_index;
  const auto fingerprint = digest_ephemeral_token(canonical + "|" + std::to_string(seed));
  return {room.members()[winner_index].player_id,
          room.members()[loser_index].player_id,
          rule_version,
          seed,
          fingerprint};
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
      io_context, std::move(*room.value), std::move(room_server), std::move(credentials), std::move(notifier),
      std::move(settlement_gateway), std::move(closed_callback), std::move(observer_store))));
}

RoomActor::RoomActor(asio::io_context& io_context,
                     domain::Room room,
                     std::string room_server,
                     std::unordered_map<std::string, JoinCredential> credentials,
                     PlayerNotifier notifier,
                     std::shared_ptr<settlement::SettlementGateway> settlement_gateway,
                     ClosedCallback closed_callback,
                     std::shared_ptr<observer::ObserverStore> observer_store)
    : strand_(asio::make_strand(io_context)),
      room_(std::move(room)),
      room_server_(std::move(room_server)),
      credentials_(std::move(credentials)),
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
    if (self->room_.state() == domain::RoomState::kSettling) {
      self->begin_settlement(std::move(completion));
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
        error = self->room_.freeze_result(
            generate_result(self->room_, self->settlement_gateway_->score_rule_version()));
      }
      if (error) {
        completion(Result<wire::Envelope>::failure(std::move(error)));
        return;
      }
      self->begin_settlement(std::move(completion));
      return;
    }

    auto payload = self->make_state_payload();
    auto response = response_envelope(wire::ROOM_STATE_CHANGED);
    protocol::set_payload(response, payload);
    completion(Result<wire::Envelope>::success(std::move(response)));
    self->notify_state();
  });
}

void RoomActor::begin_settlement(Completion completion) {
  if (!room_.result().has_value()) {
    completion(Result<wire::Envelope>::failure(make_error(ErrorCode::kSettlementNotAllowed)));
    return;
  }
  if (settlement_in_flight_) {
    completion(Result<wire::Envelope>::failure(make_error(ErrorCode::kServerBusy)));
    return;
  }
  settlement_in_flight_ = true;
  settlement::SettlementCommand command;
  command.settlement_id = "settlement-" + room_.match_id();
  command.match_id = room_.match_id();
  command.room_id = room_.room_id();
  command.result = *room_.result();
  settlement_gateway_->async_settle(
      strand_, std::move(command),
      [self = shared_from_this(), completion = std::move(completion)](
          Result<settlement::SettlementReceipt> result) mutable {
        self->complete_settlement(std::move(completion), std::move(result));
      });
}

void RoomActor::complete_settlement(Completion completion, Result<settlement::SettlementReceipt> result) {
  settlement_in_flight_ = false;
  if (!result.ok()) {
    if (observer_store_) {
      observer_store_->publish_event("settlement", "settlement.failed", room_.match_id(),
                                     room_.room_id(), result.error.message, "error");
    }
    completion(Result<wire::Envelope>::failure(std::move(result.error)));
    notify_state();
    return;
  }
  if (auto error = room_.mark_settlement_committed()) {
    completion(Result<wire::Envelope>::failure(std::move(error)));
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
  auto payload = make_state_payload();
  auto response = response_envelope(wire::ROOM_STATE_CHANGED);
  protocol::set_payload(response, payload);
  completion(Result<wire::Envelope>::success(std::move(response)));
  notify_state();
  close_once();
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
  asio::dispatch(strand_, [self = shared_from_this()] { self->stopped_ = true; });
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
