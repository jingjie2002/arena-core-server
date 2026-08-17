#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/core/result.hpp"
#include "arena_core/domain/clock.hpp"
#include "arena_core/session/token_generator.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace arena_core::session {

class Session;

struct Authentication {
  std::string session_token;
  std::string reconnect_token;
  std::uint64_t generation{1};
  bool resumed{false};
};

class SessionRegistry {
 public:
  explicit SessionRegistry(std::shared_ptr<TokenGenerator> token_generator);

  Result<Authentication> open_new(std::string player_id, domain::TimePoint now);
  Result<Authentication> resume(std::string player_id,
                                std::string reconnect_token,
                                domain::TimePoint now);
  void mark_disconnected(std::string_view reconnect_token,
                         std::uint64_t generation,
                         domain::TimePoint reconnect_deadline);
  void revoke(std::string_view reconnect_token, std::uint64_t generation);
  void bind_transport(std::string_view reconnect_token,
                      std::uint64_t generation,
                      std::weak_ptr<Session> transport);
  std::shared_ptr<Session> find_transport(std::string_view player_id) const;
  std::size_t sweep_expired(domain::TimePoint now);
  std::size_t size() const;

 private:
  struct Record {
    std::string player_id;
    std::string reconnect_token;
    std::uint64_t generation{1};
    bool connected{true};
    domain::TimePoint reconnect_deadline{};
    std::weak_ptr<Session> transport;
  };

  struct ExpiredRecord {
    std::string player_id;
    domain::TimePoint forget_after{};
  };

  void erase_record_locked(const std::string& reconnect_token);
  void remember_expired_locked(const std::string& reconnect_token,
                               const std::string& player_id,
                               domain::TimePoint now);

  std::shared_ptr<TokenGenerator> token_generator_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, Record> records_by_token_;
  std::unordered_map<std::string, std::string> token_by_player_;
  std::unordered_map<std::string, ExpiredRecord> expired_by_token_;
};

}  // namespace arena_core::session
