#include "arena_core/qa/robot_scenario.hpp"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace arena_core::qa {
namespace {

TEST(RobotScenarioTest, ParsesBoundedDeterministicScenario) {
  const nlohmann::json value = {
      {"name", "qa-replay"}, {"host", "127.0.0.1"}, {"port", 49110},
      {"seed", 42}, {"pair_count", 4}, {"rounds_per_pair", 2},
      {"duration_seconds", 5}, {"heartbeat_interval_ms", 500},
      {"queue_stagger_ms", 2}, {"match_wait_ms", 3000}, {"mmr_base", 1000}, {"mmr_stride", 100},
      {"mmr_bucket_count", 4}};
  const auto result = parse_robot_scenario(value);
  ASSERT_TRUE(result.ok()) << result.error.message;
  EXPECT_EQ(result.value->name, "qa-replay");
  EXPECT_EQ(result.value->pair_count, 4U);
  EXPECT_EQ(result.value->rounds_per_pair, 2U);
  EXPECT_EQ(result.value->seed, 42U);
}

TEST(RobotScenarioTest, RejectsUnsafeOrAmbiguousScenario) {
  auto no_pairs = parse_robot_scenario({{"pair_count", 0}});
  EXPECT_FALSE(no_pairs.ok());

  auto no_work = parse_robot_scenario({{"pair_count", 1}, {"rounds_per_pair", 0},
                                       {"duration_seconds", 0}});
  EXPECT_FALSE(no_work.ok());

  auto overflowing_mmr = parse_robot_scenario({{"pair_count", 2}, {"mmr_base", 9900},
                                                {"mmr_stride", 200}, {"mmr_bucket_count", 2}});
  EXPECT_FALSE(overflowing_mmr.ok());
}

TEST(RobotScenarioTest, ProducesNearestRankLatencyPercentiles) {
  const auto summary = summarize_latencies({1'000, 2'000, 3'000, 4'000, 100'000});
  EXPECT_DOUBLE_EQ(summary.p50_ms, 3.0);
  EXPECT_DOUBLE_EQ(summary.p95_ms, 100.0);
  EXPECT_DOUBLE_EQ(summary.p99_ms, 100.0);
  EXPECT_DOUBLE_EQ(summary.maximum_ms, 100.0);
}

TEST(RobotScenarioTest, ReplayComparisonIgnoresTimingButRejectsBusinessDrift) {
  RobotRunReport expected;
  expected.operations.push_back({0, 0, "a", "b", "match-1", "room-1", "a", "b",
                                 1016, 984, "fresh", 1'000});
  RobotRunReport actual = expected;
  actual.passed = true;
  actual.operations.front().latency_us = 50'000;
  actual.operations.front().match_id = "match-2";
  actual.operations.front().room_id = "room-2";
  EXPECT_FALSE(verify_robot_replay(expected, actual));

  actual.operations.front().winner_id = "b";
  EXPECT_TRUE(verify_robot_replay(expected, actual));

  actual = expected;
  actual.passed = true;
  actual.operations.front().winner_score = 1032;
  EXPECT_TRUE(verify_robot_replay(expected, actual));
}

}  // namespace
}  // namespace arena_core::qa
