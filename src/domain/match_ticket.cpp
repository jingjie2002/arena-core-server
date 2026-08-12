#include "arena_core/domain/match_ticket.hpp"

#include "arena_core/domain/identifiers.hpp"

#include <utility>

namespace arena_core::domain {

std::string_view match_ticket_state_name(const MatchTicketState state) noexcept {
  switch (state) {
    case MatchTicketState::kQueued: return "Queued";
    case MatchTicketState::kMatched: return "Matched";
    case MatchTicketState::kCancelled: return "Cancelled";
    case MatchTicketState::kExpired: return "Expired";
  }
  return "Expired";
}

MatchTicket::MatchTicket(std::string ticket_id,
                         std::string player_id,
                         std::string mode,
                         const std::int32_t mmr,
                         const TimePoint queued_at,
                         const TimePoint expires_at)
    : ticket_id_(std::move(ticket_id)),
      player_id_(std::move(player_id)),
      mode_(std::move(mode)),
      mmr_(mmr),
      queued_at_(queued_at),
      expires_at_(expires_at) {}

Result<MatchTicket> MatchTicket::create(std::string ticket_id,
                                        std::string player_id,
                                        std::string mode,
                                        const std::int32_t mmr,
                                        const TimePoint queued_at,
                                        const std::chrono::milliseconds max_wait) {
  if (!is_valid_resource_id(ticket_id)) {
    return Result<MatchTicket>::failure(make_error(ErrorCode::kMatchTicketNotFound, "ticket id is invalid"));
  }
  if (!is_valid_player_id(player_id)) {
    return Result<MatchTicket>::failure(make_error(ErrorCode::kSessionPlayerIdInvalid, "player id is invalid"));
  }
  if (mode != "duel") {
    return Result<MatchTicket>::failure(make_error(ErrorCode::kMatchModeUnsupported));
  }
  if (mmr < 0 || mmr > 10'000) {
    return Result<MatchTicket>::failure(make_error(ErrorCode::kMatchMmrInvalid));
  }
  if (max_wait < std::chrono::seconds(1) || max_wait > std::chrono::minutes(10)) {
    return Result<MatchTicket>::failure(make_error(ErrorCode::kMatchWaitInvalid));
  }

  return Result<MatchTicket>::success(MatchTicket(std::move(ticket_id), std::move(player_id), std::move(mode), mmr,
                                                  queued_at, queued_at + max_wait));
}

Error MatchTicket::mark_matched(std::string match_id, const TimePoint now) {
  if (expire_if_due(now)) {
    return make_error(ErrorCode::kMatchTicketExpired);
  }
  if (state_ != MatchTicketState::kQueued) {
    return make_error(ErrorCode::kMatchTicketNotQueued);
  }
  if (!is_valid_resource_id(match_id)) {
    return make_error(ErrorCode::kMatchTicketNotFound, "match id is invalid");
  }
  match_id_ = std::move(match_id);
  state_ = MatchTicketState::kMatched;
  return {};
}

Error MatchTicket::cancel(const std::string_view requesting_player, const TimePoint now) {
  if (requesting_player != player_id_) {
    return make_error(ErrorCode::kMatchTicketNotFound);
  }
  if (expire_if_due(now)) {
    return make_error(ErrorCode::kMatchTicketExpired);
  }
  if (state_ == MatchTicketState::kCancelled) {
    return {};
  }
  if (state_ != MatchTicketState::kQueued) {
    return make_error(ErrorCode::kMatchTicketNotQueued);
  }
  state_ = MatchTicketState::kCancelled;
  return {};
}

bool MatchTicket::expire_if_due(const TimePoint now) {
  if (state_ != MatchTicketState::kQueued || now < expires_at_) {
    return false;
  }
  state_ = MatchTicketState::kExpired;
  return true;
}

}  // namespace arena_core::domain
