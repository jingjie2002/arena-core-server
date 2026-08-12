#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/core/result.hpp"

#include <cstdint>
#include <string>

namespace arena_core::qa {

struct DemoBattleAdapterConfig {
  std::string host{"127.0.0.1"};
  std::uint16_t port{9200};
  std::string service_token;
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

class DemoBattleAdapter final {
 public:
  explicit DemoBattleAdapter(DemoBattleAdapterConfig config);

  Error submit(const DemoBattleResult& result) const;

 private:
  DemoBattleAdapterConfig config_;
};

std::string current_utc_rfc3339();

}  // namespace arena_core::qa
