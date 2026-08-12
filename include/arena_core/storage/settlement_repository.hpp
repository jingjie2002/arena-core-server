#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/core/result.hpp"
#include "arena_core/rank/rank_cache.hpp"
#include "arena_core/settlement/settlement_gateway.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace arena_core::storage {

struct PreparedSettlement {
  settlement::SettlementCommand command;
  std::int32_t winner_delta{0};
  std::int32_t loser_delta{0};
  std::string result_json;
};

struct DirtyRankRecord {
  std::string leaderboard;
  std::string player_id;
  std::int32_t target_score{0};
  std::string settlement_id;
  std::uint32_t attempts{0};
};

class SettlementRepository {
 public:
  virtual ~SettlementRepository() = default;

  virtual Result<settlement::SettlementReceipt> settle(const PreparedSettlement& settlement) = 0;
  virtual Result<settlement::SettlementReceipt> find_result(const settlement::SettlementQuery& query) = 0;
  virtual Result<std::vector<DirtyRankRecord>> pending_dirty(std::size_t limit) = 0;
  virtual Error resolve_dirty(const DirtyRankRecord& record) = 0;
  virtual Error resolve_dirty_score(const std::string& leaderboard, const rank::RankScore& score) = 0;
  virtual Error fail_dirty(const DirtyRankRecord& record, const std::string& message) = 0;
  virtual Result<std::vector<rank::RankScore>> all_scores() = 0;
};

struct MySqlConfig {
  std::string uri{"tcp://127.0.0.1:3306"};
  std::string user{"arena"};
  std::string password;
  std::string schema{"arena"};
};

class MySqlSettlementRepository final : public SettlementRepository {
 public:
  explicit MySqlSettlementRepository(MySqlConfig config);

  Result<settlement::SettlementReceipt> settle(const PreparedSettlement& settlement) override;
  Result<settlement::SettlementReceipt> find_result(const settlement::SettlementQuery& query) override;
  Result<std::vector<DirtyRankRecord>> pending_dirty(std::size_t limit) override;
  Error resolve_dirty(const DirtyRankRecord& record) override;
  Error resolve_dirty_score(const std::string& leaderboard, const rank::RankScore& score) override;
  Error fail_dirty(const DirtyRankRecord& record, const std::string& message) override;
  Result<std::vector<rank::RankScore>> all_scores() override;
  Error check_health();

 private:
  MySqlConfig config_;
};

}  // namespace arena_core::storage
