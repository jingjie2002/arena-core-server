#include "arena_core/domain/room.hpp"

#include "arena_core/domain/identifiers.hpp"

#include <algorithm>
#include <utility>

namespace arena_core::domain {

std::string_view room_state_name(const RoomState state) noexcept {
  switch (state) {
    case RoomState::kCreated: return "Created";
    case RoomState::kWaiting: return "Waiting";
    case RoomState::kReady: return "Ready";
    case RoomState::kRunning: return "Running";
    case RoomState::kSettling: return "Settling";
    case RoomState::kClosed: return "Closed";
    case RoomState::kAborted: return "Aborted";
  }
  return "Aborted";
}

Room::Room(std::string room_id, std::string match_id, std::string first_player, std::string second_player)
    : room_id_(std::move(room_id)),
      match_id_(std::move(match_id)),
      members_{{RoomMember{std::move(first_player), false, false},
                RoomMember{std::move(second_player), false, false}}} {}

Result<Room> Room::create(std::string room_id,
                          std::string match_id,
                          std::string first_player,
                          std::string second_player) {
  if (!is_valid_resource_id(room_id) || !is_valid_resource_id(match_id)) {
    return Result<Room>::failure(make_error(ErrorCode::kRoomNotFound, "room or match id is invalid"));
  }
  if (!is_valid_player_id(first_player) || !is_valid_player_id(second_player) || first_player == second_player) {
    return Result<Room>::failure(make_error(ErrorCode::kSettlementPlayerSetInvalid, "duel requires two players"));
  }
  return Result<Room>::success(
      Room(std::move(room_id), std::move(match_id), std::move(first_player), std::move(second_player)));
}

RoomMember* Room::find_member(const std::string_view player_id) noexcept {
  const auto it = std::find_if(members_.begin(), members_.end(), [player_id](const RoomMember& member) {
    return member.player_id == player_id;
  });
  return it == members_.end() ? nullptr : &*it;
}

bool Room::all_joined() const noexcept {
  return std::all_of(members_.begin(), members_.end(), [](const RoomMember& member) { return member.joined; });
}

bool Room::all_ready() const noexcept {
  return std::all_of(members_.begin(), members_.end(), [](const RoomMember& member) {
    return member.joined && member.ready;
  });
}

Error Room::join(const std::string_view player_id) {
  if (state_ == RoomState::kRunning || state_ == RoomState::kSettling || state_ == RoomState::kClosed) {
    return make_error(ErrorCode::kRoomAlreadyStarted);
  }
  if (state_ == RoomState::kAborted) {
    return make_error(ErrorCode::kRoomInvalidState);
  }
  auto* member = find_member(player_id);
  if (member == nullptr) {
    return make_error(ErrorCode::kRoomPlayerNotAssigned);
  }
  member->joined = true;
  if (state_ == RoomState::kCreated) {
    state_ = RoomState::kWaiting;
  }
  return {};
}

Error Room::set_ready(const std::string_view player_id) {
  if (state_ != RoomState::kWaiting && state_ != RoomState::kReady) {
    return make_error(ErrorCode::kRoomInvalidState);
  }
  auto* member = find_member(player_id);
  if (member == nullptr) {
    return make_error(ErrorCode::kRoomPlayerNotAssigned);
  }
  if (!member->joined) {
    return make_error(ErrorCode::kRoomNotJoined);
  }
  member->ready = true;
  if (all_ready()) {
    state_ = RoomState::kReady;
  }
  return {};
}

Error Room::start() {
  if (state_ == RoomState::kRunning) {
    return {};
  }
  if (state_ != RoomState::kReady || !all_joined() || !all_ready()) {
    return make_error(ErrorCode::kRoomInvalidState, "room is not ready");
  }
  state_ = RoomState::kRunning;
  return {};
}

Error Room::freeze_result(FrozenRoomResult result) {
  if (state_ == RoomState::kSettling && result_.has_value() && result_->fingerprint == result.fingerprint) {
    return {};
  }
  if (state_ != RoomState::kRunning) {
    return make_error(ErrorCode::kSettlementNotAllowed);
  }
  if (result.winner_id == result.loser_id || find_member(result.winner_id) == nullptr ||
      find_member(result.loser_id) == nullptr || result.rule_version.empty() || result.fingerprint.empty()) {
    return make_error(ErrorCode::kSettlementPlayerSetInvalid);
  }
  result_ = std::move(result);
  state_ = RoomState::kSettling;
  return {};
}

Error Room::mark_settlement_committed() {
  if (state_ == RoomState::kClosed) {
    return {};
  }
  if (state_ != RoomState::kSettling || !result_.has_value()) {
    return make_error(ErrorCode::kRoomInvalidState);
  }
  state_ = RoomState::kClosed;
  return {};
}

Error Room::abort() {
  if (state_ == RoomState::kAborted) {
    return {};
  }
  if (state_ == RoomState::kRunning || state_ == RoomState::kSettling || state_ == RoomState::kClosed) {
    return make_error(ErrorCode::kRoomInvalidState, "room cannot abort after start");
  }
  state_ = RoomState::kAborted;
  return {};
}

}  // namespace arena_core::domain
