#include "arena_core/rank/rank_cache.hpp"
#include "arena_core/rules/rule_engine.hpp"
#include "arena_core/settlement/persistent_settlement_service.hpp"
#include "arena_core/storage/settlement_repository.hpp"
#include "arena_core/storage/storage_executor.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

namespace arena_core::settlement {
namespace {

using namespace std::chrono_literals;

std::string environment(const char* name, const std::string& fallback = {}) {
  const auto* value = std::getenv(name);
  return value == nullptr ? fallback : std::string(value);
}

std::uint16_t environment_port(const char* name, const std::uint16_t fallback) {
  const auto value = environment(name);
  return value.empty() ? fallback : static_cast<std::uint16_t>(std::stoul(value));
}

class AsyncHarness {
 public:
  AsyncHarness(std::shared_ptr<storage::SettlementRepository> repository,
               std::shared_ptr<rank::RankCache> cache,
               std::shared_ptr<rules::RuleEngine> rules,
               std::shared_ptr<storage::StorageExecutor> executor)
      : service(std::make_shared<PersistentSettlementService>(io, PersistentSettlementOptions{},
                                                              std::move(executor), std::move(repository),
                                                              std::move(cache), std::move(rules))),
        guard(boost::asio::make_work_guard(io)),
        thread([this] { io.run(); }) {}

  ~AsyncHarness() {
    service->stop();
    guard.reset();
    io.stop();
    thread.join();
  }

  Result<SettlementReceipt> settle(SettlementCommand command) {
    std::promise<Result<SettlementReceipt>> promise;
    auto future = promise.get_future();
    service->async_settle(io.get_executor(), std::move(command),
                          [&promise](Result<SettlementReceipt> result) { promise.set_value(std::move(result)); });
    if (future.wait_for(5s) != std::future_status::ready) {
      return Result<SettlementReceipt>::failure(make_error(ErrorCode::kInternalError, "integration timeout"));
    }
    return future.get();
  }

  Result<rank::RankView> query(const std::string& player) {
    std::promise<Result<rank::RankView>> promise;
    auto future = promise.get_future();
    service->async_query_rank(io.get_executor(), {"duel:season-1", player, 2},
                              [&promise](Result<rank::RankView> result) { promise.set_value(std::move(result)); });
    return future.get();
  }

  Error repair() {
    std::promise<Error> promise;
    auto future = promise.get_future();
    service->async_repair_once(io.get_executor(), [&promise](Error error) { promise.set_value(std::move(error)); });
    return future.get();
  }

  Error rebuild() {
    std::promise<Error> promise;
    auto future = promise.get_future();
    service->async_rebuild_rank(io.get_executor(), "duel:season-1",
                                [&promise](Error error) { promise.set_value(std::move(error)); });
    return future.get();
  }

  boost::asio::io_context io;
  std::shared_ptr<PersistentSettlementService> service;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard;
  std::thread thread;
};

SettlementCommand make_command(const std::string& suffix,
                               const std::string& fingerprint,
                               const std::string& first = "a",
                               const std::string& second = "b") {
  SettlementCommand command;
  command.settlement_id = "settlement-it-" + suffix;
  command.match_id = "match-it-" + suffix;
  command.room_id = "room-it-" + suffix;
  command.result = domain::FrozenRoomResult{"player-" + first + "-" + suffix,
                                            "player-" + second + "-" + suffix,
                                            "score-v1",
                                            42,
                                            fingerprint};
  return command;
}

TEST(DataIntegrationTest, MySqlFactRedisCacheIdempotencyConflictRepairAndRebuild) {
  const auto mysql_uri = environment("ARENA_M5_MYSQL_URI");
  if (mysql_uri.empty()) {
    GTEST_SKIP() << "ARENA_M5_MYSQL_URI is not configured";
  }
  storage::MySqlConfig mysql{mysql_uri,
                             environment("ARENA_M5_MYSQL_USER", "arena_test"),
                             environment("ARENA_M5_MYSQL_PASSWORD", "arena_test"),
                             environment("ARENA_M5_MYSQL_SCHEMA", "arena_m5")};
  rank::RedisRankConfig redis;
  redis.host = environment("ARENA_M5_REDIS_HOST", "127.0.0.1");
  redis.port = environment_port("ARENA_M5_REDIS_PORT", 6392);
  redis.database = 15;
  const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  redis.key_prefix = "arena:m5-it:" + suffix + ":";

  auto repository = std::make_shared<storage::MySqlSettlementRepository>(mysql);
  ASSERT_FALSE(repository->check_health());
  auto rules = rules::LuaRuleEngine::load(std::filesystem::path(ARENA_SOURCE_DIR) / "rules/match_rule.lua",
                                          std::filesystem::path(ARENA_SOURCE_DIR) / "rules/score_rule.lua");
  ASSERT_TRUE(rules.ok()) << rules.error.message;
  auto executor = std::make_shared<storage::StorageExecutor>(2, 32);
  auto cache = std::make_shared<rank::HiredisRankCache>(redis);

  const auto rollback_suffix = suffix + "-rollback";
  storage::PreparedSettlement invalid_json;
  invalid_json.command = make_command(
      rollback_suffix, "2222222222222222222222222222222222222222", "rollback-a", "rollback-b");
  invalid_json.winner_delta = 16;
  invalid_json.loser_delta = -16;
  invalid_json.result_json = "{invalid-json";
  const auto rolled_back = repository->settle(invalid_json);
  ASSERT_FALSE(rolled_back.ok());
  EXPECT_EQ(rolled_back.error.code, ErrorCode::kSettlementStorageFailed);
  const auto scores_after_rollback = repository->all_scores();
  ASSERT_TRUE(scores_after_rollback.ok());
  EXPECT_EQ(std::count_if(scores_after_rollback.value->begin(), scores_after_rollback.value->end(),
                          [&](const rank::RankScore& score) {
                            return score.player_id == invalid_json.command.result.winner_id ||
                                   score.player_id == invalid_json.command.result.loser_id;
                          }),
            0);

  const auto tied_board = "duel:tie-" + suffix;
  ASSERT_FALSE(cache->rebuild(tied_board, {{"tie-b", 1'000}, {"tie-a", 1'000}, {"tie-c", 900}}));
  const auto tied_rank = cache->query({tied_board, "tie-b", 1});
  ASSERT_TRUE(tied_rank.ok()) << tied_rank.error.message;
  EXPECT_EQ(tied_rank.value->player.rank, 2U);
  ASSERT_EQ(tied_rank.value->neighbors.size(), 2U);
  EXPECT_EQ(tied_rank.value->neighbors.front().player_id, "tie-a");

  {
    AsyncHarness good(repository, cache, *rules.value, executor);
    const auto command = make_command(suffix, "0123456789012345678901234567890123456789");
    auto first = good.settle(command);
    ASSERT_TRUE(first.ok()) << first.error.message;
    EXPECT_FALSE(first.value->idempotent);
    EXPECT_EQ(first.value->cache_state, "fresh");

    auto duplicate = good.settle(command);
    ASSERT_TRUE(duplicate.ok()) << duplicate.error.message;
    EXPECT_TRUE(duplicate.value->idempotent);
    EXPECT_EQ(duplicate.value->scores[0].score, first.value->scores[0].score);

    auto conflict_command = command;
    conflict_command.result.fingerprint = "abcdefabcdefabcdefabcdefabcdefabcdefabcd";
    const auto conflict = good.settle(std::move(conflict_command));
    ASSERT_FALSE(conflict.ok());
    EXPECT_EQ(conflict.error.code, ErrorCode::kSettlementConflict);

    const auto rank_result = good.query(command.result.winner_id);
    ASSERT_TRUE(rank_result.ok()) << rank_result.error.message;
    EXPECT_EQ(rank_result.value->player.score, 1'016);
  }

  auto unavailable_redis = redis;
  unavailable_redis.port = static_cast<std::uint16_t>(redis.port + 1U);
  auto unavailable_cache = std::make_shared<rank::HiredisRankCache>(unavailable_redis);
  const auto dirty_suffix = suffix + "-dirty";
  {
    AsyncHarness degraded(repository, unavailable_cache, *rules.value, executor);
    auto degraded_result = degraded.settle(
        make_command(dirty_suffix, "1111111111111111111111111111111111111111", "c", "d"));
    ASSERT_TRUE(degraded_result.ok()) << degraded_result.error.message;
    EXPECT_EQ(degraded_result.value->cache_state, "stale");
  }
  const auto pending_before = repository->pending_dirty(100);
  ASSERT_TRUE(pending_before.ok());
  EXPECT_GE(pending_before.value->size(), 2U);

  {
    AsyncHarness repaired(repository, cache, *rules.value, executor);
    EXPECT_FALSE(repaired.repair());
    auto repaired_rank = repaired.query("player-c-" + dirty_suffix);
    ASSERT_TRUE(repaired_rank.ok()) << repaired_rank.error.message;
    EXPECT_EQ(repaired_rank.value->player.score, 1'016);
    EXPECT_FALSE(repaired.rebuild());
  }
  const auto pending_after = repository->pending_dirty(100);
  ASSERT_TRUE(pending_after.ok());
  EXPECT_TRUE(pending_after.value->empty());
  executor->stop();
}

}  // namespace
}  // namespace arena_core::settlement
