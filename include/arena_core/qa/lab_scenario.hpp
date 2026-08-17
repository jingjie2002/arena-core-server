#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/core/result.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace arena_core::qa {

enum class LabScenarioKind {
  kCompleteMatch,
  kReconnectRecovery,
  kDuplicatePlayerRequest,
  kDuplicateTrustedResult,
  kRedisFailureRecovery,
};

struct LabScenarioRequest {
  std::uint32_t schema_version{1};
  LabScenarioKind kind{LabScenarioKind::kCompleteMatch};
  std::string name{"complete_match"};
  std::uint64_t seed{0};
  std::uint32_t timeout_ms{60'000};
  nlohmann::json parameters = nlohmann::json::object();
};

struct LabRunEnvironment {
  std::string player_host{"127.0.0.1"};
  std::uint16_t player_port{9'100};
  std::string result_host{"127.0.0.1"};
  std::uint16_t result_port{9'200};
  std::string result_token;
  std::string observer_host{"127.0.0.1"};
  std::uint16_t observer_port{8'080};
  std::string toxiproxy_host;
  std::uint16_t toxiproxy_port{8'474};
  std::string toxiproxy_proxy_name{"arena_lab_redis"};
  std::string toxiproxy_listen{"0.0.0.0:26379"};
  std::string toxiproxy_upstream{"redis:6379"};
  std::uint32_t reconnect_window_ms{15'000};
  std::string git_commit{"unknown"};
  std::string service_config_digest{"unknown"};
};

Result<LabScenarioRequest> parse_lab_scenario_request(const nlohmann::json& value);
Result<LabScenarioRequest> parse_lab_run_request(const nlohmann::json& value);
Result<LabScenarioKind> parse_lab_scenario_kind(const std::string& name);
std::string lab_scenario_name(LabScenarioKind kind);
std::vector<std::string> lab_scenario_names();
nlohmann::json lab_scenario_schema(LabScenarioKind kind);
nlohmann::json lab_capabilities(const LabRunEnvironment& environment, bool redis_fault_available);
nlohmann::json lab_scenario_request_json(const LabScenarioRequest& request);

}  // namespace arena_core::qa
