#include "arena_core/rank/rank_cache.hpp"
#include "arena_core/rules/rule_engine.hpp"
#include "arena_core/settlement/persistent_settlement_service.hpp"
#include "arena_core/storage/settlement_repository.hpp"
#include "arena_core/storage/storage_executor.hpp"

#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

namespace arena_core::settlement {
namespace {

using namespace std::chrono_literals;

class FakeRepository final : public storage::SettlementRepository {
 public:
  Result<SettlementReceipt> settle(const storage::PreparedSettlement& prepared) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++settle_calls;
    if (fail_settle) {
      return Result<SettlementReceipt>::failure(make_error(ErrorCode::kSettlementStorageFailed));
    }
    const auto existing = receipts.find(prepared.command.match_id);
    if (existing != receipts.end()) {
      if (existing->second.fingerprint != prepared.command.result.fingerprint) {
        return Result<SettlementReceipt>::failure(make_error(ErrorCode::kSettlementConflict));
      }
      auto receipt = existing->second;
      receipt.idempotent = true;
      return Result<SettlementReceipt>::success(std::move(receipt));
    }
    auto score = [&](const std::string& player) -> std::int32_t& {
      auto [iterator, inserted] = scores.emplace(player, 1'000);
      static_cast<void>(inserted);
      return iterator->second;
    };
    auto& winner = score(prepared.command.result.winner_id);
    auto& loser = score(prepared.command.result.loser_id);
    const auto winner_before = winner;
    const auto loser_before = loser;
    winner += prepared.winner_delta;
    loser += prepared.loser_delta;
    SettlementReceipt receipt;
    receipt.settlement_id = prepared.command.settlement_id;
    receipt.match_id = prepared.command.match_id;
    receipt.fingerprint = prepared.command.result.fingerprint;
    receipt.rule_version = prepared.command.result.rule_version;
    receipt.scores = {{prepared.command.result.winner_id, winner},
                      {prepared.command.result.loser_id, loser}};
    receipt.idempotent = false;
    receipt.cache_state = "stale";
    receipt.room_id = prepared.command.room_id;
    receipt.winner_id = prepared.command.result.winner_id;
    receipt.loser_id = prepared.command.result.loser_id;
    receipt.finished_at = prepared.command.result.finished_at;
    receipt.players = {{receipt.winner_id, winner_before, winner, "win"},
                       {receipt.loser_id, loser_before, loser, "loss"}};
    receipts.emplace(prepared.command.match_id, receipt);
    for (const auto& value : receipt.scores) {
      dirty[{prepared.command.leaderboard, value.player_id}] = storage::DirtyRankRecord{
          prepared.command.leaderboard, value.player_id, value.score, receipt.settlement_id, 0};
    }
    return Result<SettlementReceipt>::success(std::move(receipt));
  }

  Result<SettlementReceipt> find_result(const SettlementQuery& query) override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [match_id, receipt] : receipts) {
      if ((!query.match_id.empty() && query.match_id == match_id) ||
          (!query.settlement_id.empty() && query.settlement_id == receipt.settlement_id)) {
        return Result<SettlementReceipt>::success(receipt);
      }
    }
    return Result<SettlementReceipt>::failure(make_error(ErrorCode::kSettlementNotFound));
  }

  Result<std::vector<storage::DirtyRankRecord>> pending_dirty(std::size_t limit) override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<storage::DirtyRankRecord> result;
    for (const auto& [key, record] : dirty) {
      static_cast<void>(key);
      if (result.size() == limit) {
        break;
      }
      result.push_back(record);
    }
    return Result<std::vector<storage::DirtyRankRecord>>::success(std::move(result));
  }

  Error resolve_dirty(const storage::DirtyRankRecord& record) override {
    std::lock_guard<std::mutex> lock(mutex_);
    dirty.erase({record.leaderboard, record.player_id});
    return {};
  }

  Error resolve_dirty_score(const std::string& leaderboard, const rank::RankScore& score) override {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = dirty.find({leaderboard, score.player_id});
    if (found != dirty.end() && found->second.target_score == score.score) {
      dirty.erase(found);
    }
    return {};
  }

  Error fail_dirty(const storage::DirtyRankRecord& record, const std::string&) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = dirty.find({record.leaderboard, record.player_id});
    if (found != dirty.end()) {
      ++found->second.attempts;
    }
    return {};
  }

  Result<std::vector<rank::RankScore>> all_scores() override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<rank::RankScore> result;
    for (const auto& [player, value] : scores) {
      result.push_back({player, value});
    }
    return Result<std::vector<rank::RankScore>>::success(std::move(result));
  }

  std::size_t dirty_size() {
    std::lock_guard<std::mutex> lock(mutex_);
    return dirty.size();
  }

  bool fail_settle{false};
  int settle_calls{0};
  std::mutex mutex_;
  std::map<std::string, SettlementReceipt> receipts;
  std::map<std::string, std::int32_t> scores;
  std::map<std::pair<std::string, std::string>, storage::DirtyRankRecord> dirty;
};

class ServiceHarness {
 public:
  ServiceHarness()
      : executor(std::make_shared<storage::StorageExecutor>(1, 8)),
        repository(std::make_shared<FakeRepository>()),
        cache(std::make_shared<rank::InMemoryRankCache>()),
        rules(std::make_shared<rules::BuiltInRuleEngine>()),
        service(std::make_shared<PersistentSettlementService>(io, PersistentSettlementOptions{}, executor,
                                                              repository, cache, rules)),
        guard(boost::asio::make_work_guard(io)),
        thread([this] { io.run(); }) {}

  ~ServiceHarness() {
    service->stop();
    executor->stop();
    guard.reset();
    io.stop();
    thread.join();
  }

  Result<SettlementReceipt> settle(SettlementCommand command) {
    std::promise<Result<SettlementReceipt>> promise;
    auto future = promise.get_future();
    service->async_settle(io.get_executor(), std::move(command),
                          [&promise](Result<SettlementReceipt> result) { promise.set_value(std::move(result)); });
    if (future.wait_for(2s) != std::future_status::ready) {
      return Result<SettlementReceipt>::failure(make_error(ErrorCode::kInternalError, "test timeout"));
    }
    return future.get();
  }

  Error repair() {
    std::promise<Error> promise;
    auto future = promise.get_future();
    service->async_repair_once(io.get_executor(), [&promise](Error error) { promise.set_value(std::move(error)); });
    return future.get();
  }

  Result<rank::RankView> query(std::string player) {
    std::promise<Result<rank::RankView>> promise;
    auto future = promise.get_future();
    service->async_query_rank(io.get_executor(), rank::RankQuery{"duel:season-1", std::move(player), 2},
                              [&promise](Result<rank::RankView> result) { promise.set_value(std::move(result)); });
    return future.get();
  }

  Result<SettlementReceipt> query_result(SettlementQuery query) {
    std::promise<Result<SettlementReceipt>> promise;
    auto future = promise.get_future();
    service->async_query_result(
        io.get_executor(), std::move(query),
        [&promise](Result<SettlementReceipt> result) { promise.set_value(std::move(result)); });
    return future.get();
  }

  boost::asio::io_context io;
  std::shared_ptr<storage::StorageExecutor> executor;
  std::shared_ptr<FakeRepository> repository;
  std::shared_ptr<rank::InMemoryRankCache> cache;
  std::shared_ptr<rules::BuiltInRuleEngine> rules;
  std::shared_ptr<PersistentSettlementService> service;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard;
  std::thread thread;
};

SettlementCommand command(std::string match_id, std::string fingerprint = "fingerprint-1") {
  SettlementCommand value;
  value.settlement_id = "settlement-" + match_id;
  value.match_id = std::move(match_id);
  value.room_id = "room-1";
  value.result = domain::FrozenRoomResult{"player-1", "player-2", "score-v1", 42,
                                          std::move(fingerprint), "2026-08-12T05:30:00Z"};
  return value;
}

TEST(SettlementServiceTest, CommitsOnceUpdatesRankAndTreatsSameFingerprintAsIdempotent) {
  ServiceHarness harness;
  auto first = harness.settle(command("match-1"));
  ASSERT_TRUE(first.ok()) << first.error.message;
  EXPECT_FALSE(first.value->idempotent);
  EXPECT_EQ(first.value->cache_state, "fresh");
  EXPECT_EQ(harness.repository->dirty_size(), 0U);

  auto duplicate = harness.settle(command("match-1"));
  ASSERT_TRUE(duplicate.ok());
  EXPECT_TRUE(duplicate.value->idempotent);
  EXPECT_EQ(harness.repository->scores["player-1"], 1'016);
  EXPECT_EQ(harness.repository->scores["player-2"], 984);

  auto rank = harness.query("player-1");
  ASSERT_TRUE(rank.ok());
  EXPECT_EQ(rank.value->player.rank, 1U);
  EXPECT_EQ(rank.value->player.score, 1'016);

  const auto by_match = harness.query_result({"", "match-1"});
  ASSERT_TRUE(by_match.ok());
  EXPECT_EQ(by_match.value->settlement_id, "settlement-match-1");
  EXPECT_EQ(by_match.value->winner_id, "player-1");
  const auto by_result = harness.query_result({"settlement-match-1", ""});
  ASSERT_TRUE(by_result.ok());
  EXPECT_EQ(by_result.value->players.size(), 2U);
}

TEST(SettlementServiceTest, RejectsConflictingFingerprintWithoutSecondScoreWrite) {
  ServiceHarness harness;
  ASSERT_TRUE(harness.settle(command("match-1")).ok());
  const auto conflict = harness.settle(command("match-1", "different-fingerprint"));
  ASSERT_FALSE(conflict.ok());
  EXPECT_EQ(conflict.error.code, ErrorCode::kSettlementConflict);
  EXPECT_EQ(harness.repository->scores["player-1"], 1'016);
}

TEST(SettlementServiceTest, MySqlFailureDoesNotTouchRankCache) {
  ServiceHarness harness;
  harness.repository->fail_settle = true;
  const auto failed = harness.settle(command("match-1"));
  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error.code, ErrorCode::kSettlementStorageFailed);
  const auto rank = harness.query("player-1");
  EXPECT_FALSE(rank.ok());
  EXPECT_EQ(rank.error.code, ErrorCode::kRankPlayerNotFound);
}

TEST(SettlementServiceTest, RedisFailureLeavesDirtyEvidenceAndRepairReplaysTargetScores) {
  ServiceHarness harness;
  harness.cache->fail_writes(true);
  auto result = harness.settle(command("match-1"));
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value->cache_state, "stale");
  EXPECT_EQ(harness.repository->dirty_size(), 2U);

  harness.cache->fail_writes(false);
  EXPECT_FALSE(harness.repair());
  EXPECT_EQ(harness.repository->dirty_size(), 0U);
  const auto rank = harness.query("player-2");
  ASSERT_TRUE(rank.ok());
  EXPECT_EQ(rank.value->player.score, 984);
}

TEST(SettlementServiceTest, RepeatedDependencyFaultsRecoverWithoutHalfSettlement) {
  ServiceHarness harness;
  for (std::uint32_t index = 0; index < 20U; ++index) {
    const auto match_id = "fault-sequence-" + std::to_string(index);
    if (index % 5U == 0U) {
      harness.repository->fail_settle = true;
      const auto failed = harness.settle(command(match_id));
      ASSERT_FALSE(failed.ok());
      EXPECT_EQ(failed.error.code, ErrorCode::kSettlementStorageFailed);
      harness.repository->fail_settle = false;
    }

    const auto inject_cache_failure = index % 3U == 0U;
    harness.cache->fail_writes(inject_cache_failure);
    const auto settled = harness.settle(command(match_id));
    ASSERT_TRUE(settled.ok()) << settled.error.message;
    EXPECT_EQ(settled.value->cache_state, inject_cache_failure ? "stale" : "fresh");
    if (inject_cache_failure) {
      EXPECT_GT(harness.repository->dirty_size(), 0U);
      harness.cache->fail_writes(false);
      EXPECT_FALSE(harness.repair());
    }
  }

  EXPECT_EQ(harness.repository->dirty_size(), 0U);
  const auto rank = harness.query("player-1");
  ASSERT_TRUE(rank.ok());
  EXPECT_EQ(rank.value->player.score, 1'320);
  EXPECT_EQ(harness.repository->scores["player-2"], 680);
}

TEST(RankCacheTest, SortsEqualScoresByPlayerIdAndBuildsStableNeighbors) {
  rank::InMemoryRankCache cache;
  ASSERT_FALSE(cache.upsert("duel:season-1", {{"player-b", 1'000}, {"player-a", 1'000}, {"player-c", 900}}));
  auto result = cache.query({"duel:season-1", "player-b", 1});
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value->player.rank, 2U);
  ASSERT_EQ(result.value->neighbors.size(), 2U);
  EXPECT_EQ(result.value->neighbors[0].player_id, "player-a");
  EXPECT_EQ(result.value->neighbors[1].player_id, "player-c");
}

}  // namespace
}  // namespace arena_core::settlement
