#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/core/result.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace arena_core::qa {

struct RobotScenario {
  std::string name{"full-flow"};
  std::string host{"127.0.0.1"};
  std::uint16_t port{49110};
  std::uint64_t seed{20260812};
  std::uint32_t pair_count{1};
  std::uint32_t rounds_per_pair{1};
  std::uint32_t duration_seconds{0};
  std::uint32_t heartbeat_interval_ms{1'000};
  std::uint32_t queue_stagger_ms{0};
  std::uint32_t match_wait_ms{5'000};
  std::int32_t mmr_base{1'000};
  std::int32_t mmr_stride{100};
  std::uint32_t mmr_bucket_count{1};
};

struct RobotOperation {
  std::uint32_t pair_index{0};
  std::uint32_t round_index{0};
  std::string first_player;
  std::string second_player;
  std::string match_id;
  std::string room_id;
  std::string winner_id;
  std::string loser_id;
  std::int32_t winner_score{0};
  std::int32_t loser_score{0};
  std::string cache_state;
  std::uint64_t latency_us{0};
};

struct RobotFailure {
  std::uint32_t pair_index{0};
  std::string message;
};

struct LatencySummary {
  double p50_ms{0.0};
  double p95_ms{0.0};
  double p99_ms{0.0};
  double maximum_ms{0.0};
};

struct RobotRunReport {
  RobotScenario scenario;
  bool passed{false};
  std::uint64_t started_unix_ms{0};
  std::uint64_t elapsed_ms{0};
  std::uint64_t successful_connections{0};
  std::uint64_t heartbeat_count{0};
  double throughput_rounds_per_second{0.0};
  LatencySummary latency;
  std::vector<RobotOperation> operations;
  std::vector<RobotFailure> failures;
  std::string replay_source;
  bool replay_verified{false};
};

Result<RobotScenario> parse_robot_scenario(const nlohmann::json& value);
Result<RobotScenario> load_robot_scenario(const std::string& path);
Result<RobotRunReport> load_robot_report(const std::string& path);

RobotRunReport run_robot_scenario(const RobotScenario& scenario);
LatencySummary summarize_latencies(const std::vector<std::uint64_t>& latency_us);
Error verify_robot_replay(const RobotRunReport& expected, const RobotRunReport& actual);

nlohmann::json robot_scenario_json(const RobotScenario& scenario);
nlohmann::json robot_report_json(const RobotRunReport& report);
Error write_robot_report(const std::string& path, const RobotRunReport& report);

}  // namespace arena_core::qa
