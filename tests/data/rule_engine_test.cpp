#include "arena_core/rules/rule_engine.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

namespace arena_core::rules {
namespace {

std::filesystem::path source_path(const std::string& relative) {
  return std::filesystem::path(ARENA_SOURCE_DIR) / relative;
}

TEST(RuleEngineTest, LoadsVersionedLuaRulesAndUsesExactBoundaries) {
  const auto engine = LuaRuleEngine::load(source_path("rules/match_rule.lua"), source_path("rules/score_rule.lua"));
  ASSERT_TRUE(engine.ok()) << engine.error.message;
  EXPECT_EQ((*engine.value)->match_rule_version(), "match-v1");
  EXPECT_EQ((*engine.value)->score_rule_version(), "score-v1");

  const auto negative = (*engine.value)->allowed_mmr_delta(-1);
  const auto zero = (*engine.value)->allowed_mmr_delta(0);
  const auto one = (*engine.value)->allowed_mmr_delta(1);
  const auto capped = (*engine.value)->allowed_mmr_delta(1'000);
  ASSERT_TRUE(negative.ok() && zero.ok() && one.ok() && capped.ok());
  EXPECT_EQ(*negative.value, 100);
  EXPECT_EQ(*zero.value, 100);
  EXPECT_EQ(*one.value, 125);
  EXPECT_EQ(*capped.value, 1'000);
}

TEST(RuleEngineTest, ScoreIsDeterministicAndVersionChecked) {
  const auto engine = LuaRuleEngine::load(source_path("rules/match_rule.lua"), source_path("rules/score_rule.lua"));
  ASSERT_TRUE(engine.ok());
  const auto first = (*engine.value)->score(42, "player-1", "player-2", "score-v1");
  const auto second = (*engine.value)->score(42, "player-1", "player-2", "score-v1");
  ASSERT_TRUE(first.ok() && second.ok());
  EXPECT_EQ(first.value->winner_delta, 16);
  EXPECT_EQ(first.value->loser_delta, -16);
  EXPECT_EQ(first.value->winner_delta, second.value->winner_delta);

  const auto mismatch = (*engine.value)->score(42, "player-1", "player-2", "score-v2");
  ASSERT_FALSE(mismatch.ok());
  EXPECT_EQ(mismatch.error.code, ErrorCode::kRuleVersionMismatch);
}

TEST(RuleEngineTest, RejectsInvalidOrCapabilitySeekingScript) {
  const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto invalid = std::filesystem::temp_directory_path() /
                       ("arena-invalid-rule-" + std::to_string(suffix) + ".lua");
  {
    std::ofstream output(invalid);
    output << "RULE_VERSION='score-v1'\nfunction score() return os.time(), -1 end\n";
  }
  const auto engine = LuaRuleEngine::load(source_path("rules/match_rule.lua"), invalid);
  EXPECT_FALSE(engine.ok());
  EXPECT_EQ(engine.error.code, ErrorCode::kRuleLoadFailed);
  std::error_code ignored;
  std::filesystem::remove(invalid, ignored);
}

TEST(RuleEngineTest, StopsScriptThatExceedsInstructionBudget) {
  const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto looping = std::filesystem::temp_directory_path() /
                       ("arena-looping-rule-" + std::to_string(suffix) + ".lua");
  {
    std::ofstream output(looping);
    output << "RULE_VERSION='score-v1'\nfunction score() while true do end end\n";
  }
  const auto engine = LuaRuleEngine::load(source_path("rules/match_rule.lua"), looping);
  EXPECT_FALSE(engine.ok());
  EXPECT_EQ(engine.error.code, ErrorCode::kRuleLoadFailed);
  std::error_code ignored;
  std::filesystem::remove(looping, ignored);
}

}  // namespace
}  // namespace arena_core::rules
