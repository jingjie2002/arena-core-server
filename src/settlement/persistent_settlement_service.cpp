#include "arena_core/settlement/persistent_settlement_service.hpp"

#include <utility>

#include <nlohmann/json.hpp>

namespace arena_core::settlement {
namespace {

namespace asio = boost::asio;

template <typename Completion, typename Value>
void post_result(asio::any_io_executor executor, Completion completion, Result<Value> result) {
  asio::post(executor, [completion = std::move(completion), result = std::move(result)]() mutable {
    completion(std::move(result));
  });
}

void post_error(asio::any_io_executor executor,
                PersistentSettlementService::MaintenanceCompletion completion,
                Error error) {
  asio::post(executor, [completion = std::move(completion), error = std::move(error)]() mutable {
    completion(std::move(error));
  });
}

}  // namespace

PersistentSettlementService::PersistentSettlementService(
    asio::io_context& io_context,
    PersistentSettlementOptions options,
    std::shared_ptr<storage::StorageExecutor> executor,
    std::shared_ptr<storage::SettlementRepository> repository,
    std::shared_ptr<rank::RankCache> rank_cache,
    std::shared_ptr<rules::RuleEngine> rules)
    : strand_(asio::make_strand(io_context)),
      repair_timer_(strand_),
      options_(std::move(options)),
      executor_(std::move(executor)),
      repository_(std::move(repository)),
      rank_cache_(std::move(rank_cache)),
      rules_(std::move(rules)) {}

void PersistentSettlementService::start() {
  asio::dispatch(strand_, [self = shared_from_this()] {
    if (self->started_) {
      return;
    }
    self->started_ = true;
    self->stopped_.store(false);
    self->arm_repair();
  });
}

void PersistentSettlementService::stop() {
  stopped_.store(true);
  asio::dispatch(strand_, [self = shared_from_this()] { self->repair_timer_.cancel(); });
}

void PersistentSettlementService::async_settle(asio::any_io_executor reply_executor,
                                                SettlementCommand command,
                                                SettlementCompletion completion) {
  if (stopped_.load()) {
    post_result(std::move(reply_executor), std::move(completion),
                Result<SettlementReceipt>::failure(make_error(ErrorCode::kServerShuttingDown)));
    return;
  }
  auto adjustment = rules_->score(command.result.deterministic_seed, command.result.winner_id,
                                  command.result.loser_id,
                                  command.result.rule_version);
  if (!adjustment.ok()) {
    post_result(std::move(reply_executor), std::move(completion),
                Result<SettlementReceipt>::failure(std::move(adjustment.error)));
    return;
  }
  auto prepared = prepare(std::move(command), *adjustment.value);
  const auto submitted = executor_->try_submit(
      [self = shared_from_this(), reply_executor, completion,
       prepared = std::move(prepared)]() mutable {
        auto result = self->repository_->settle(prepared);
        if (result.ok()) {
          std::lock_guard<std::mutex> cache_lock(self->cache_write_mutex_);
          auto cache_error = self->rank_cache_->upsert(prepared.command.leaderboard, result.value->scores);
          if (!cache_error) {
            for (const auto& score : result.value->scores) {
              const storage::DirtyRankRecord dirty{prepared.command.leaderboard, score.player_id, score.score,
                                                   result.value->settlement_id, 0};
              static_cast<void>(self->repository_->resolve_dirty(dirty));
            }
            result.value->cache_state = "fresh";
          } else {
            result.value->cache_state = "stale";
          }
        }
        post_result(std::move(reply_executor), std::move(completion), std::move(result));
      });
  if (!submitted) {
    post_result(std::move(reply_executor), std::move(completion),
                Result<SettlementReceipt>::failure(make_error(ErrorCode::kServerBusy)));
  }
}

void PersistentSettlementService::async_query_rank(asio::any_io_executor reply_executor,
                                                    rank::RankQuery query,
                                                    RankCompletion completion) {
  if (stopped_.load()) {
    post_result(std::move(reply_executor), std::move(completion),
                Result<rank::RankView>::failure(make_error(ErrorCode::kServerShuttingDown)));
    return;
  }
  const auto submitted = executor_->try_submit(
      [self = shared_from_this(), reply_executor, completion,
       query = std::move(query)]() mutable {
        auto result = self->rank_cache_->query(query);
        post_result(std::move(reply_executor), std::move(completion), std::move(result));
      });
  if (!submitted) {
    post_result(std::move(reply_executor), std::move(completion),
                Result<rank::RankView>::failure(make_error(ErrorCode::kServerBusy)));
  }
}

Result<std::int32_t> PersistentSettlementService::allowed_mmr_delta(const std::int64_t waited_seconds) const {
  return rules_->allowed_mmr_delta(waited_seconds);
}

const std::string& PersistentSettlementService::score_rule_version() const noexcept {
  return rules_->score_rule_version();
}

void PersistentSettlementService::async_repair_once(asio::any_io_executor reply_executor,
                                                     MaintenanceCompletion completion) {
  const auto submitted = executor_->try_submit(
      [self = shared_from_this(), reply_executor, completion]() mutable {
        post_error(std::move(reply_executor), std::move(completion), self->repair_once());
      });
  if (!submitted) {
    post_error(std::move(reply_executor), std::move(completion), make_error(ErrorCode::kServerBusy));
  }
}

void PersistentSettlementService::async_rebuild_rank(asio::any_io_executor reply_executor,
                                                      std::string leaderboard,
                                                      MaintenanceCompletion completion) {
  const auto submitted = executor_->try_submit(
      [self = shared_from_this(), reply_executor, completion,
       leaderboard = std::move(leaderboard)]() mutable {
        std::lock_guard<std::mutex> cache_lock(self->cache_write_mutex_);
        auto scores = self->repository_->all_scores();
        if (!scores.ok()) {
          post_error(std::move(reply_executor), std::move(completion), std::move(scores.error));
          return;
        }
        auto error = self->rank_cache_->rebuild(leaderboard, *scores.value);
        if (!error) {
          for (const auto& score : *scores.value) {
            error = self->repository_->resolve_dirty_score(leaderboard, score);
            if (error) {
              break;
            }
          }
        }
        post_error(std::move(reply_executor), std::move(completion), std::move(error));
      });
  if (!submitted) {
    post_error(std::move(reply_executor), std::move(completion), make_error(ErrorCode::kServerBusy));
  }
}

storage::PreparedSettlement PersistentSettlementService::prepare(
    SettlementCommand command, const rules::ScoreAdjustment& adjustment) const {
  nlohmann::json result{{"winner_id", command.result.winner_id},
                        {"loser_id", command.result.loser_id},
                        {"seed", command.result.deterministic_seed},
                        {"fingerprint", command.result.fingerprint},
                        {"rule_version", command.result.rule_version},
                        {"winner_delta", adjustment.winner_delta},
                        {"loser_delta", adjustment.loser_delta}};
  return storage::PreparedSettlement{std::move(command), adjustment.winner_delta, adjustment.loser_delta,
                                     result.dump()};
}

void PersistentSettlementService::arm_repair() {
  if (stopped_.load()) {
    return;
  }
  repair_timer_.expires_after(options_.repair_interval);
  repair_timer_.async_wait(asio::bind_executor(strand_, [self = shared_from_this()](const boost::system::error_code& error) {
    if (error || self->stopped_.load()) {
      return;
    }
    if (!self->repair_in_flight_.exchange(true)) {
      if (!self->executor_->try_submit([self] {
            static_cast<void>(self->repair_once());
            self->repair_in_flight_.store(false);
          })) {
        self->repair_in_flight_.store(false);
      }
    }
    self->arm_repair();
  }));
}

Error PersistentSettlementService::repair_once() {
  auto pending = repository_->pending_dirty(options_.repair_batch_size);
  if (!pending.ok()) {
    return std::move(pending.error);
  }
  Error first_error;
  for (const auto& record : *pending.value) {
    std::lock_guard<std::mutex> cache_lock(cache_write_mutex_);
    auto error = rank_cache_->upsert(record.leaderboard, {{record.player_id, record.target_score}});
    if (!error) {
      error = repository_->resolve_dirty(record);
    } else {
      static_cast<void>(repository_->fail_dirty(record, error.message));
    }
    if (error && !first_error) {
      first_error = std::move(error);
    }
  }
  return first_error;
}

}  // namespace arena_core::settlement
