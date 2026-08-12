#include "arena_core/session/session_registry.hpp"

#include "arena_core/domain/identifiers.hpp"
#include "arena_core/session/session.hpp"

#include <utility>

namespace arena_core::session {

SessionRegistry::SessionRegistry(std::shared_ptr<TokenGenerator> token_generator)
    : token_generator_(std::move(token_generator)) {}

Result<Authentication> SessionRegistry::open_new(std::string player_id, const domain::TimePoint now) {
  if (!domain::is_valid_player_id(player_id)) {
    return Result<Authentication>::failure(make_error(ErrorCode::kSessionPlayerIdInvalid));
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (const auto player = token_by_player_.find(player_id); player != token_by_player_.end()) {
    const auto record = records_by_token_.find(player->second);
    if (record != records_by_token_.end() && (record->second.connected || now < record->second.reconnect_deadline)) {
      return Result<Authentication>::failure(
          make_error(ErrorCode::kSessionNotActive, "player already has an active or reconnectable session"));
    }
    erase_record_locked(player->second);
  }

  const auto reconnect_token = token_generator_->next_token();
  const auto session_token = token_generator_->next_token();
  records_by_token_.emplace(reconnect_token, Record{player_id, reconnect_token, 1, true, {}, {}});
  token_by_player_[player_id] = reconnect_token;
  return Result<Authentication>::success(Authentication{session_token, reconnect_token, 1, false});
}

Result<Authentication> SessionRegistry::resume(std::string player_id,
                                                std::string reconnect_token,
                                                const domain::TimePoint now) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto existing = records_by_token_.find(reconnect_token);
  if (existing == records_by_token_.end() || existing->second.player_id != player_id) {
    return Result<Authentication>::failure(make_error(ErrorCode::kSessionTokenInvalid));
  }
  if (existing->second.connected) {
    return Result<Authentication>::failure(make_error(ErrorCode::kSessionTokenInvalid, "session is still connected"));
  }
  if (now >= existing->second.reconnect_deadline) {
    erase_record_locked(reconnect_token);
    return Result<Authentication>::failure(make_error(ErrorCode::kSessionReconnectExpired));
  }

  const auto generation = existing->second.generation + 1;
  const auto new_reconnect_token = token_generator_->next_token();
  const auto new_session_token = token_generator_->next_token();
  records_by_token_.erase(existing);
  records_by_token_.emplace(new_reconnect_token,
                            Record{player_id, new_reconnect_token, generation, true, {}, {}});
  token_by_player_[player_id] = new_reconnect_token;
  return Result<Authentication>::success(
      Authentication{new_session_token, new_reconnect_token, generation, true});
}

void SessionRegistry::mark_disconnected(const std::string_view reconnect_token,
                                        const std::uint64_t generation,
                                        const domain::TimePoint reconnect_deadline) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto record = records_by_token_.find(std::string(reconnect_token));
  if (record == records_by_token_.end() || record->second.generation != generation) {
    return;
  }
  record->second.connected = false;
  record->second.reconnect_deadline = reconnect_deadline;
}

void SessionRegistry::revoke(const std::string_view reconnect_token, const std::uint64_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto record = records_by_token_.find(std::string(reconnect_token));
  if (record == records_by_token_.end() || record->second.generation != generation) {
    return;
  }
  erase_record_locked(record->first);
}

void SessionRegistry::bind_transport(const std::string_view reconnect_token,
                                     const std::uint64_t generation,
                                     std::weak_ptr<Session> transport) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto record = records_by_token_.find(std::string(reconnect_token));
  if (record == records_by_token_.end() || record->second.generation != generation) {
    return;
  }
  record->second.transport = std::move(transport);
}

std::shared_ptr<Session> SessionRegistry::find_transport(const std::string_view player_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto player = token_by_player_.find(std::string(player_id));
  if (player == token_by_player_.end()) {
    return {};
  }
  const auto record = records_by_token_.find(player->second);
  if (record == records_by_token_.end() || !record->second.connected) {
    return {};
  }
  return record->second.transport.lock();
}

std::size_t SessionRegistry::sweep_expired(const domain::TimePoint now) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t removed = 0;
  for (auto iterator = records_by_token_.begin(); iterator != records_by_token_.end();) {
    if (!iterator->second.connected && now >= iterator->second.reconnect_deadline) {
      token_by_player_.erase(iterator->second.player_id);
      iterator = records_by_token_.erase(iterator);
      ++removed;
    } else {
      ++iterator;
    }
  }
  return removed;
}

std::size_t SessionRegistry::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return records_by_token_.size();
}

void SessionRegistry::erase_record_locked(const std::string& reconnect_token) {
  const auto record = records_by_token_.find(reconnect_token);
  if (record == records_by_token_.end()) {
    return;
  }
  token_by_player_.erase(record->second.player_id);
  records_by_token_.erase(record);
}

}  // namespace arena_core::session
