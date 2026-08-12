#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/core/error.hpp"
#include "arena_core/core/result.hpp"
#include "arena_core/domain/clock.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace arena_core::domain {

enum class MatchTicketState { kQueued, kMatched, kCancelled, kExpired };

std::string_view match_ticket_state_name(MatchTicketState state) noexcept;

class MatchTicket {
 public:
  static Result<MatchTicket> create(std::string ticket_id,
                                    std::string player_id,
                                    std::string mode,
                                    std::int32_t mmr,
                                    TimePoint queued_at,
                                    std::chrono::milliseconds max_wait);

  Error mark_matched(std::string match_id, TimePoint now);
  Error cancel(std::string_view requesting_player, TimePoint now);
  bool expire_if_due(TimePoint now);

  const std::string& ticket_id() const noexcept { return ticket_id_; }
  const std::string& player_id() const noexcept { return player_id_; }
  const std::string& mode() const noexcept { return mode_; }
  std::int32_t mmr() const noexcept { return mmr_; }
  MatchTicketState state() const noexcept { return state_; }
  TimePoint queued_at() const noexcept { return queued_at_; }
  TimePoint expires_at() const noexcept { return expires_at_; }
  const std::optional<std::string>& match_id() const noexcept { return match_id_; }

 private:
  MatchTicket(std::string ticket_id,
              std::string player_id,
              std::string mode,
              std::int32_t mmr,
              TimePoint queued_at,
              TimePoint expires_at);

  std::string ticket_id_;
  std::string player_id_;
  std::string mode_;
  std::int32_t mmr_{0};
  MatchTicketState state_{MatchTicketState::kQueued};
  TimePoint queued_at_;
  TimePoint expires_at_;
  std::optional<std::string> match_id_;
};

}  // namespace arena_core::domain
