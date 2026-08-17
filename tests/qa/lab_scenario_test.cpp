#include "arena_core/qa/lab_scenario.hpp"

#include <gtest/gtest.h>

namespace arena_core::qa {
namespace {

TEST(LabScenarioTest, AppliesSafeDefaultsAndRejectsEnvironmentFields) {
  auto valid = parse_lab_scenario_request({{"scenario", "complete_match"},
                                           {"parameters", {{"player_count", 8}}}});
  ASSERT_TRUE(valid.ok()) << valid.error.message;
  EXPECT_EQ(valid.value->parameters.at("player_count"), 8);
  EXPECT_EQ(valid.value->parameters.at("mmr_mode"), "uniform");

  auto host = parse_lab_scenario_request({{"scenario", "complete_match"},
                                          {"host", "attacker.invalid"},
                                          {"parameters", {{"player_count", 2}}}});
  EXPECT_FALSE(host.ok());
  EXPECT_EQ(host.error.code, ErrorCode::kQaScenarioInvalid);

  auto token = parse_lab_scenario_request({{"scenario", "complete_match"},
                                           {"parameters", {{"player_count", 2},
                                                           {"token", "secret"}}}});
  EXPECT_FALSE(token.ok());
}

TEST(LabScenarioTest, EnforcesScenarioBoundsAndCombinationRules) {
  EXPECT_FALSE(parse_lab_scenario_request({{"scenario", "complete_match"},
      {"parameters", {{"player_count", 3}}}}).ok());
  EXPECT_FALSE(parse_lab_scenario_request({{"scenario", "complete_match"},
      {"parameters", {{"player_count", 32}, {"mmr_mode", "stair"},
                      {"mmr_base", 9000}, {"mmr_stride", 1000}}}}).ok());
  EXPECT_FALSE(parse_lab_scenario_request({{"scenario", "duplicate_trusted_result"},
      {"parameters", {{"repeat_count", 11}}}}).ok());
  EXPECT_FALSE(parse_lab_scenario_request({{"scenario", "redis_failure_recovery"},
      {"parameters", {{"fault_duration_ms", 500}}}}).ok());
  EXPECT_FALSE(parse_lab_scenario_request({{"scenario", "redis_failure_recovery"},
      {"parameters", {{"fault_kind", "latency"}, {"fault_value_ms", 0}}}}).ok());
}

TEST(LabScenarioTest, CapabilitiesNeverExposeRuntimeAddressOrToken) {
  LabRunEnvironment environment;
  environment.player_host = "private-host";
  environment.result_token = "super-secret";
  const auto capabilities = lab_capabilities(environment, false);
  const auto encoded = capabilities.dump();
  EXPECT_EQ(encoded.find("private-host"), std::string::npos);
  EXPECT_EQ(encoded.find("super-secret"), std::string::npos);
  const auto& scenarios = capabilities.at("scenarios");
  ASSERT_EQ(scenarios.size(), 5U);
  EXPECT_FALSE(scenarios.back().at("available").get<bool>());
}

}  // namespace
}  // namespace arena_core::qa
