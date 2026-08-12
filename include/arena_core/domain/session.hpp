#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/core/error.hpp"
#include "arena_core/domain/clock.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace arena_core::domain {

enum class SessionState { kConnected, kActive, kReconnecting, kClosed };

std::string_view session_state_name(SessionState state) noexcept;

class SessionStateMachine {
 public:
  struct Durations {
    std::chrono::milliseconds hello_deadline{std::chrono::seconds(5)};
    std::chrono::milliseconds idle_timeout{std::chrono::seconds(30)};
    std::chrono::milliseconds reconnect_window{std::chrono::seconds(15)};
  };

  explicit SessionStateMachine(TimePoint connected_at);
  SessionStateMachine(TimePoint connected_at, Durations durations);

  Error activate(std::string player_id, TimePoint now);
  Error record_activity(TimePoint now);
  Error connection_lost(TimePoint now);
  Error resume(std::string_view player_id, TimePoint now);
  bool expire_if_due(TimePoint now);
  void close() noexcept;

  SessionState state() const noexcept { return state_; }
  const std::optional<std::string>& player_id() const noexcept { return player_id_; }
  std::uint64_t generation() const noexcept { return generation_; }
  TimePoint deadline() const noexcept { return deadline_; }

 private:
  SessionState state_{SessionState::kConnected};
  Durations durations_;
  std::optional<std::string> player_id_;
  TimePoint last_activity_;
  TimePoint deadline_;
  std::uint64_t generation_{1};
};

}  // namespace arena_core::domain
