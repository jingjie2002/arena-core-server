#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/core/result.hpp"
#include "arena_core/domain/room.hpp"
#include "arena_core/rank/rank_cache.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio/any_io_executor.hpp>

namespace arena_core::settlement {

struct SettlementCommand {
  std::string settlement_id;
  std::string match_id;
  std::string room_id;
  std::string leaderboard{"duel:season-1"};
  domain::FrozenRoomResult result;
};

struct SettlementReceipt {
  std::string settlement_id;
  std::string match_id;
  std::string fingerprint;
  std::string rule_version;
  std::vector<rank::RankScore> scores;
  bool idempotent{false};
  std::string cache_state{"fresh"};
};

class SettlementGateway {
 public:
  using SettlementCompletion = std::function<void(Result<SettlementReceipt>)>;
  using RankCompletion = std::function<void(Result<rank::RankView>)>;

  virtual ~SettlementGateway() = default;
  virtual void start() {}
  virtual void stop() {}
  virtual void async_settle(boost::asio::any_io_executor reply_executor,
                            SettlementCommand command,
                            SettlementCompletion completion) = 0;
  virtual void async_query_rank(boost::asio::any_io_executor reply_executor,
                                rank::RankQuery query,
                                RankCompletion completion) = 0;
  virtual Result<std::int32_t> allowed_mmr_delta(std::int64_t waited_seconds) const = 0;
  virtual const std::string& score_rule_version() const noexcept = 0;
};

std::shared_ptr<SettlementGateway> make_in_memory_settlement_gateway();

}  // namespace arena_core::settlement
