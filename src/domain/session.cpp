#include "arena_core/domain/session.hpp"

#include "arena_core/domain/identifiers.hpp"

#include <utility>

namespace arena_core::domain {

std::string_view session_state_name(const SessionState state) noexcept {
  switch (state) {
    case SessionState::kConnected: return "Connected";
    case SessionState::kActive: return "Active";
    case SessionState::kReconnecting: return "Reconnecting";
    case SessionState::kClosed: return "Closed";
  }
  return "Closed";
}

SessionStateMachine::SessionStateMachine(const TimePoint connected_at)
    : SessionStateMachine(connected_at, Durations{}) {}

SessionStateMachine::SessionStateMachine(const TimePoint connected_at, const Durations durations)
    : durations_(durations), last_activity_(connected_at), deadline_(connected_at + durations.hello_deadline) {}

Error SessionStateMachine::activate(std::string player_id, const TimePoint now) {
  if (state_ != SessionState::kConnected) {
    return make_error(ErrorCode::kSessionNotActive, "Hello is only valid for a connected session");
  }
  if (now >= deadline_) {
    state_ = SessionState::kClosed;
    return make_error(ErrorCode::kSessionHelloRequired, "Hello deadline expired");
  }
  if (!is_valid_player_id(player_id)) {
    return make_error(ErrorCode::kSessionPlayerIdInvalid, "player id is invalid");
  }

  player_id_ = std::move(player_id);
  state_ = SessionState::kActive;
  last_activity_ = now;
  deadline_ = now + durations_.idle_timeout;
  return {};
}

Error SessionStateMachine::record_activity(const TimePoint now) {
  if (state_ != SessionState::kActive) {
    return make_error(ErrorCode::kSessionNotActive, "session is not active");
  }
  if (now >= deadline_) {
    state_ = SessionState::kClosed;
    return make_error(ErrorCode::kSessionNotActive, "session idle timeout expired");
  }
  last_activity_ = now;
  deadline_ = now + durations_.idle_timeout;
  return {};
}

Error SessionStateMachine::connection_lost(const TimePoint now) {
  if (state_ == SessionState::kClosed || state_ == SessionState::kReconnecting) {
    return {};
  }
  if (state_ != SessionState::kActive) {
    return make_error(ErrorCode::kSessionNotActive, "only an active session can reconnect");
  }
  state_ = SessionState::kReconnecting;
  deadline_ = now + durations_.reconnect_window;
  return {};
}

Error SessionStateMachine::resume(const std::string_view player_id, const TimePoint now) {
  if (state_ != SessionState::kReconnecting) {
    return make_error(ErrorCode::kSessionNotActive, "session is not waiting for reconnect");
  }
  if (now >= deadline_) {
    state_ = SessionState::kClosed;
    return make_error(ErrorCode::kSessionReconnectExpired, "reconnect window expired");
  }
  if (!player_id_.has_value() || *player_id_ != player_id) {
    return make_error(ErrorCode::kSessionTokenInvalid, "reconnect identity mismatch");
  }
  state_ = SessionState::kActive;
  ++generation_;
  last_activity_ = now;
  deadline_ = now + durations_.idle_timeout;
  return {};
}

bool SessionStateMachine::expire_if_due(const TimePoint now) {
  if (state_ == SessionState::kClosed || now < deadline_) {
    return false;
  }
  state_ = SessionState::kClosed;
  return true;
}

void SessionStateMachine::close() noexcept { state_ = SessionState::kClosed; }

}  // namespace arena_core::domain
