#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/core/error.hpp"
#include "arena_core/core/result.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace arena_core::domain {

enum class RoomState { kCreated, kWaiting, kReady, kRunning, kSettling, kClosed, kAborted };

std::string_view room_state_name(RoomState state) noexcept;

struct RoomMember {
  std::string player_id;
  bool joined{false};
  bool ready{false};
};

struct FrozenRoomResult {
  std::string winner_id;
  std::string loser_id;
  std::string rule_version;
  std::uint64_t deterministic_seed{0};
  std::string fingerprint;
};

class Room {
 public:
  static Result<Room> create(std::string room_id,
                             std::string match_id,
                             std::string first_player,
                             std::string second_player);

  Error join(std::string_view player_id);
  Error set_ready(std::string_view player_id);
  Error start();
  Error freeze_result(FrozenRoomResult result);
  Error mark_settlement_committed();
  Error abort();

  RoomState state() const noexcept { return state_; }
  const std::string& room_id() const noexcept { return room_id_; }
  const std::string& match_id() const noexcept { return match_id_; }
  const std::array<RoomMember, 2>& members() const noexcept { return members_; }
  const std::optional<FrozenRoomResult>& result() const noexcept { return result_; }

 private:
  Room(std::string room_id, std::string match_id, std::string first_player, std::string second_player);
  RoomMember* find_member(std::string_view player_id) noexcept;
  bool all_joined() const noexcept;
  bool all_ready() const noexcept;

  std::string room_id_;
  std::string match_id_;
  std::array<RoomMember, 2> members_;
  RoomState state_{RoomState::kCreated};
  std::optional<FrozenRoomResult> result_;
};

}  // namespace arena_core::domain
