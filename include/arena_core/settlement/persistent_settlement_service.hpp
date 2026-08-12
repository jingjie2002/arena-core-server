#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/rank/rank_cache.hpp"
#include "arena_core/rules/rule_engine.hpp"
#include "arena_core/settlement/settlement_gateway.hpp"
#include "arena_core/storage/settlement_repository.hpp"
#include "arena_core/storage/storage_executor.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <boost/asio.hpp>

namespace arena_core::settlement {

struct PersistentSettlementOptions {
  std::chrono::milliseconds repair_interval{std::chrono::seconds(2)};
  std::size_t repair_batch_size{64};
  std::string default_leaderboard{"duel:season-1"};
};

class PersistentSettlementService final : public SettlementGateway,
                                          public std::enable_shared_from_this<PersistentSettlementService> {
 public:
  using MaintenanceCompletion = std::function<void(Error)>;

  PersistentSettlementService(boost::asio::io_context& io_context,
                              PersistentSettlementOptions options,
                              std::shared_ptr<storage::StorageExecutor> executor,
                              std::shared_ptr<storage::SettlementRepository> repository,
                              std::shared_ptr<rank::RankCache> rank_cache,
                              std::shared_ptr<rules::RuleEngine> rules);

  void start() override;
  void stop() override;
  void async_settle(boost::asio::any_io_executor reply_executor,
                    SettlementCommand command,
                    SettlementCompletion completion) override;
  void async_query_rank(boost::asio::any_io_executor reply_executor,
                        rank::RankQuery query,
                        RankCompletion completion) override;
  void async_query_result(boost::asio::any_io_executor reply_executor,
                          SettlementQuery query,
                          ResultQueryCompletion completion) override;
  Result<std::int32_t> allowed_mmr_delta(std::int64_t waited_seconds) const override;
  const std::string& score_rule_version() const noexcept override;

  void async_repair_once(boost::asio::any_io_executor reply_executor, MaintenanceCompletion completion);
  void async_rebuild_rank(boost::asio::any_io_executor reply_executor,
                          std::string leaderboard,
                          MaintenanceCompletion completion);

 private:
  storage::PreparedSettlement prepare(SettlementCommand command,
                                      const rules::ScoreAdjustment& adjustment) const;
  void arm_repair();
  Error repair_once();

  boost::asio::strand<boost::asio::io_context::executor_type> strand_;
  boost::asio::steady_timer repair_timer_;
  PersistentSettlementOptions options_;
  std::shared_ptr<storage::StorageExecutor> executor_;
  std::shared_ptr<storage::SettlementRepository> repository_;
  std::shared_ptr<rank::RankCache> rank_cache_;
  std::shared_ptr<rules::RuleEngine> rules_;
  std::mutex cache_write_mutex_;
  std::atomic<bool> stopped_{false};
  std::atomic<bool> repair_in_flight_{false};
  bool started_{false};
};

}  // namespace arena_core::settlement
