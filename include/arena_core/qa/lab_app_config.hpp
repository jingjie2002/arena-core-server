#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/core/result.hpp"
#include "arena_core/qa/lab_scenario.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace arena_core::qa {

struct LabHttpAppConfig {
  std::string bind_address{"127.0.0.1"};
  std::uint16_t port{8'081};
  bool allow_non_loopback{false};
  std::filesystem::path static_root{"web/lab"};
  std::size_t maximum_request_body_bytes{65'536};
};

struct LabReportAppConfig {
  std::filesystem::path directory{".runtime/lab-runs"};
};

struct LabTrustedResultAppConfig {
  std::string host{"127.0.0.1"};
  std::uint16_t port{9'200};
  std::string service_token_env{"ARENA_RESULT_SERVICE_TOKEN"};
};

struct LabToxiproxyAppConfig {
  bool enabled{false};
  std::string host{"127.0.0.1"};
  std::uint16_t port{8'474};
  std::string proxy_name{"arena_lab_redis"};
  std::string listen{"0.0.0.0:26379"};
  std::string upstream{"redis:6379"};
};

struct LabAppConfig {
  std::string environment{"local-lab"};
  LabHttpAppConfig http;
  LabReportAppConfig reports;
  std::string player_host{"127.0.0.1"};
  std::uint16_t player_port{9'100};
  LabTrustedResultAppConfig trusted_result;
  std::string observer_host{"127.0.0.1"};
  std::uint16_t observer_port{8'080};
  std::uint32_t reconnect_window_ms{15'000};
  LabToxiproxyAppConfig toxiproxy;
  std::string git_commit{"unknown"};
  std::string service_config_digest{"unknown"};
};

Result<LabAppConfig> load_lab_app_config(const std::filesystem::path& path);
Error validate_lab_app_config(const LabAppConfig& config);
Result<LabRunEnvironment> lab_run_environment(const LabAppConfig& config);

}  // namespace arena_core::qa
