#include "arena_core/settlement/settlement_gateway.hpp"

#include "arena_core/rules/rule_engine.hpp"

#include <map>
#include <mutex>
#include <utility>

#include <boost/asio/post.hpp>

namespace arena_core::settlement {
namespace {

class InMemorySettlementGateway final : public SettlementGateway {
 public:
  void async_settle(boost::asio::any_io_executor reply_executor,
                    SettlementCommand command,
                    SettlementCompletion completion) override {
    Result<SettlementReceipt> result = settle(std::move(command));
    boost::asio::post(reply_executor,
                      [completion = std::move(completion), result = std::move(result)]() mutable {
                        completion(std::move(result));
                      });
  }

  void async_query_rank(boost::asio::any_io_executor reply_executor,
                        rank::RankQuery query,
                        RankCompletion completion) override {
    auto result = rank_cache_.query(query);
    boost::asio::post(reply_executor,
                      [completion = std::move(completion), result = std::move(result)]() mutable {
                        completion(std::move(result));
                      });
  }

  void async_query_result(boost::asio::any_io_executor reply_executor,
                          SettlementQuery query,
                          ResultQueryCompletion completion) override {
    Result<SettlementReceipt> result;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto match_id = query.match_id;
      if (!query.settlement_id.empty()) {
        const auto indexed = match_by_settlement_.find(query.settlement_id);
        if (indexed == match_by_settlement_.end()) {
          result = Result<SettlementReceipt>::failure(make_error(ErrorCode::kSettlementNotFound));
        } else {
          match_id = indexed->second;
        }
      }
      if (!result.error) {
        const auto existing = receipts_.find(match_id);
        result = existing == receipts_.end()
            ? Result<SettlementReceipt>::failure(make_error(ErrorCode::kSettlementNotFound))
            : Result<SettlementReceipt>::success(existing->second);
      }
    }
    boost::asio::post(reply_executor,
                      [completion = std::move(completion), result = std::move(result)]() mutable {
                        completion(std::move(result));
                      });
  }

  Result<std::int32_t> allowed_mmr_delta(const std::int64_t waited_seconds) const override {
    return rules_.allowed_mmr_delta(waited_seconds);
  }

  const std::string& score_rule_version() const noexcept override { return rules_.score_rule_version(); }

 private:
  Result<SettlementReceipt> settle(SettlementCommand command) {
    const auto adjustment = rules_.score(command.result.deterministic_seed, command.result.winner_id,
                                         command.result.loser_id,
                                         command.result.rule_version);
    if (!adjustment.ok()) {
      return Result<SettlementReceipt>::failure(std::move(adjustment.error));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto existing = receipts_.find(command.match_id);
    if (existing != receipts_.end()) {
      if (existing->second.fingerprint != command.result.fingerprint ||
          existing->second.settlement_id != command.settlement_id) {
        return Result<SettlementReceipt>::failure(make_error(ErrorCode::kSettlementConflict));
      }
      auto receipt = existing->second;
      receipt.idempotent = true;
      return Result<SettlementReceipt>::success(std::move(receipt));
    }
    if (const auto reused = match_by_settlement_.find(command.settlement_id);
        reused != match_by_settlement_.end() && reused->second != command.match_id) {
      return Result<SettlementReceipt>::failure(make_error(ErrorCode::kSettlementConflict));
    }
    auto [winner_iterator, winner_inserted] = player_scores_.emplace(command.result.winner_id, 1'000);
    auto [loser_iterator, loser_inserted] = player_scores_.emplace(command.result.loser_id, 1'000);
    static_cast<void>(winner_inserted);
    static_cast<void>(loser_inserted);
    auto& winner_score = winner_iterator->second;
    auto& loser_score = loser_iterator->second;
    const auto winner_before = winner_score;
    const auto loser_before = loser_score;
    winner_score += adjustment.value->winner_delta;
    loser_score += adjustment.value->loser_delta;
    SettlementReceipt receipt;
    receipt.settlement_id = command.settlement_id;
    receipt.match_id = command.match_id;
    receipt.fingerprint = command.result.fingerprint;
    receipt.rule_version = adjustment.value->rule_version;
    receipt.scores = {{command.result.winner_id, winner_score},
                      {command.result.loser_id, loser_score}};
    receipt.idempotent = false;
    receipt.cache_state = "fresh";
    receipt.room_id = command.room_id;
    receipt.winner_id = command.result.winner_id;
    receipt.loser_id = command.result.loser_id;
    receipt.finished_at = command.result.finished_at;
    receipt.players = {{command.result.winner_id, winner_before, winner_score, "win"},
                       {command.result.loser_id, loser_before, loser_score, "loss"}};
    rank_cache_.upsert(command.leaderboard, receipt.scores);
    receipts_[command.match_id] = receipt;
    match_by_settlement_[command.settlement_id] = command.match_id;
    return Result<SettlementReceipt>::success(std::move(receipt));
  }

  mutable rules::BuiltInRuleEngine rules_;
  std::mutex mutex_;
  std::map<std::string, SettlementReceipt> receipts_;
  std::map<std::string, std::string> match_by_settlement_;
  std::map<std::string, std::int32_t> player_scores_;
  rank::InMemoryRankCache rank_cache_;
};

}  // namespace

std::shared_ptr<SettlementGateway> make_in_memory_settlement_gateway() {
  return std::make_shared<InMemorySettlementGateway>();
}

}  // namespace arena_core::settlement
