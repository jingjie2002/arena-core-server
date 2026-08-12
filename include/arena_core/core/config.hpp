#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/core/error.hpp"
#include "arena_core/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace arena_core {

struct NetworkConfig {
  std::string bind_address{"127.0.0.1"};
  std::uint16_t tcp_port{9100};
  std::uint16_t observer_port{8080};
  std::size_t max_frame_bytes{65'536};
};

struct ExecutionConfig {
  std::size_t io_threads{2};
  std::size_t storage_threads{4};
  std::size_t storage_queue_capacity{512};
};

struct LoggingConfig {
  std::string level{"info"};
  std::string file;
};

struct SessionConfig {
  std::uint32_t hello_timeout_ms{5'000};
  std::uint32_t idle_timeout_ms{30'000};
  std::uint32_t reconnect_window_ms{15'000};
};

struct MySqlAppConfig {
  std::string uri{"tcp://127.0.0.1:3306"};
  std::string user{"arena"};
  std::string password_env{"ARENA_MYSQL_PASSWORD"};
  std::string schema{"arena"};
};

struct RedisAppConfig {
  std::string host{"127.0.0.1"};
  std::uint16_t port{6379};
  std::uint32_t database{0};
  std::string password_env{"ARENA_REDIS_PASSWORD"};
  std::string key_prefix{"arena:dev:"};
  std::uint32_t connect_timeout_ms{500};
};

struct RuleConfig {
  std::string match_script{"rules/match_rule.lua"};
  std::string score_script{"rules/score_rule.lua"};
};

struct DataConfig {
  bool enabled{false};
  MySqlAppConfig mysql;
  RedisAppConfig redis;
  RuleConfig rules;
  std::uint32_t repair_interval_ms{2'000};
  std::size_t repair_batch_size{64};
};

struct ObserverConfig {
  bool enabled{true};
  bool demo_mode{false};
  bool allow_non_loopback_demo{false};
  std::string static_root{"web/observer"};
  std::size_t max_events{2'048};
};

struct TrustedResultConfig {
  bool enabled{false};
  std::string bind_address{"127.0.0.1"};
  std::uint16_t port{9200};
  bool allow_non_loopback{false};
  std::string service_token_env{"ARENA_RESULT_SERVICE_TOKEN"};
  std::uint32_t result_wait_timeout_ms{900'000};
  std::size_t max_request_body_bytes{16'384};
};

struct AppConfig {
  std::string environment{"dev"};
  NetworkConfig network;
  ExecutionConfig execution;
  LoggingConfig logging;
  SessionConfig session;
  DataConfig data;
  ObserverConfig observer;
  TrustedResultConfig trusted_result;
};

using ConfigResult = Result<AppConfig>;

ConfigResult load_app_config(const std::filesystem::path& path);
Error validate_app_config(const AppConfig& config);

}  // namespace arena_core
