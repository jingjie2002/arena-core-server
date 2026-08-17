#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/core/result.hpp"

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace arena_core::qa {

struct DemoBattleAdapterConfig {
  std::string host{"127.0.0.1"};
  std::uint16_t port{9200};
  std::string service_token;
  std::uint32_t request_timeout_ms{5'000};
};

struct DemoBattleResult {
  std::string result_id;
  std::string match_id;
  std::string room_id;
  std::string winner_id;
  std::string loser_id;
  std::string rule_version{"score-v1"};
  std::string finished_at;
};

struct DemoBattleReceipt {
  std::uint16_t http_status{0};
  std::string status;
  std::string result_id;
  std::string match_id;
  std::string room_id;
  std::string winner_id;
  std::string loser_id;
  std::string cache_state;
  bool idempotent{false};
  nlohmann::json body = nlohmann::json::object();
};

class DemoBattleAdapter final {
 public:
  explicit DemoBattleAdapter(DemoBattleAdapterConfig config);

  Error submit(const DemoBattleResult& result) const;
  Result<DemoBattleReceipt> submit_with_receipt(const DemoBattleResult& result) const;
  Result<DemoBattleReceipt> query(const std::string& result_id) const;

 private:
  DemoBattleAdapterConfig config_;
};

std::string current_utc_rfc3339();

}  // namespace arena_core::qa
