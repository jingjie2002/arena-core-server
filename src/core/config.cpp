#include "arena_core/core/config.hpp"

#include <fstream>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

namespace arena_core {
namespace {

template <typename T>
void read_if_present(const nlohmann::json& object, const char* key, T& value) {
  if (const auto it = object.find(key); it != object.end()) {
    it->get_to(value);
  }
}

}  // namespace

Error validate_app_config(const AppConfig& config) {
  if (config.environment.empty() || config.environment.size() > 32) {
    return make_error(ErrorCode::kConfigValidationFailed, "environment length must be between 1 and 32");
  }
  if (config.network.bind_address.empty()) {
    return make_error(ErrorCode::kConfigValidationFailed, "network.bind_address must not be empty");
  }
  if (config.network.tcp_port == 0 || config.network.observer_port == 0) {
    return make_error(ErrorCode::kConfigValidationFailed, "network ports must be between 1 and 65535");
  }
  if (config.network.tcp_port == config.network.observer_port) {
    return make_error(ErrorCode::kConfigValidationFailed, "TCP and observer ports must differ");
  }
  if (config.network.max_frame_bytes == 0 || config.network.max_frame_bytes > 65'536) {
    return make_error(ErrorCode::kConfigValidationFailed, "max_frame_bytes must be between 1 and 65536");
  }
  if (config.execution.io_threads == 0 || config.execution.io_threads > 64) {
    return make_error(ErrorCode::kConfigValidationFailed, "io_threads must be between 1 and 64");
  }
  if (config.execution.storage_threads == 0 || config.execution.storage_threads > 64) {
    return make_error(ErrorCode::kConfigValidationFailed, "storage_threads must be between 1 and 64");
  }
  if (config.execution.storage_queue_capacity == 0 || config.execution.storage_queue_capacity > 65'536) {
    return make_error(ErrorCode::kConfigValidationFailed, "storage_queue_capacity must be between 1 and 65536");
  }
  if (config.session.hello_timeout_ms < 100 || config.session.hello_timeout_ms > 60'000) {
    return make_error(ErrorCode::kConfigValidationFailed, "hello_timeout_ms must be between 100 and 60000");
  }
  if (config.session.idle_timeout_ms < 100 || config.session.idle_timeout_ms > 300'000) {
    return make_error(ErrorCode::kConfigValidationFailed, "idle_timeout_ms must be between 100 and 300000");
  }
  if (config.session.reconnect_window_ms < 100 || config.session.reconnect_window_ms > 120'000) {
    return make_error(ErrorCode::kConfigValidationFailed, "reconnect_window_ms must be between 100 and 120000");
  }
  if (config.data.enabled) {
    if (config.data.mysql.uri.empty() || config.data.mysql.user.empty() || config.data.mysql.schema.empty() ||
        config.data.mysql.password_env.empty()) {
      return make_error(ErrorCode::kConfigValidationFailed, "data.mysql connection fields must not be empty");
    }
    if (config.data.redis.host.empty() || config.data.redis.port == 0 || config.data.redis.key_prefix.empty() ||
        config.data.redis.password_env.empty()) {
      return make_error(ErrorCode::kConfigValidationFailed, "data.redis connection fields are invalid");
    }
    if (config.data.redis.connect_timeout_ms < 10 || config.data.redis.connect_timeout_ms > 30'000) {
      return make_error(ErrorCode::kConfigValidationFailed,
                        "data.redis.connect_timeout_ms must be between 10 and 30000");
    }
    if (config.data.rules.match_script.empty() || config.data.rules.score_script.empty()) {
      return make_error(ErrorCode::kConfigValidationFailed, "data rule script paths must not be empty");
    }
    if (config.data.repair_interval_ms < 100 || config.data.repair_interval_ms > 300'000 ||
        config.data.repair_batch_size == 0 || config.data.repair_batch_size > 1'000) {
      return make_error(ErrorCode::kConfigValidationFailed, "data repair limits are invalid");
    }
  }
  if (config.observer.enabled) {
    if (config.observer.static_root.empty()) {
      return make_error(ErrorCode::kConfigValidationFailed, "observer.static_root must not be empty");
    }
    if (config.observer.max_events < 16 || config.observer.max_events > 100'000) {
      return make_error(ErrorCode::kConfigValidationFailed,
                        "observer.max_events must be between 16 and 100000");
    }
    const bool loopback = config.network.bind_address == "127.0.0.1" ||
                          config.network.bind_address == "::1";
    if (config.observer.demo_mode && !loopback &&
        !config.observer.allow_non_loopback_demo) {
      return make_error(ErrorCode::kConfigValidationFailed,
                        "observer demo mode requires loopback or an explicit trusted proxy override");
    }
    if (config.observer.allow_non_loopback_demo && !config.observer.demo_mode) {
      return make_error(ErrorCode::kConfigValidationFailed,
                        "observer.allow_non_loopback_demo requires observer.demo_mode");
    }
  }
  if (config.trusted_result.enabled) {
    if (config.trusted_result.bind_address.empty() || config.trusted_result.port == 0) {
      return make_error(ErrorCode::kConfigValidationFailed,
                        "trusted_result listener fields are invalid");
    }
    if (config.trusted_result.port == config.network.tcp_port ||
        (config.observer.enabled && config.trusted_result.port == config.network.observer_port)) {
      return make_error(ErrorCode::kConfigValidationFailed,
                        "trusted_result port must differ from active public ports");
    }
    const bool loopback = config.trusted_result.bind_address == "127.0.0.1" ||
                          config.trusted_result.bind_address == "::1";
    if (!loopback && !config.trusted_result.allow_non_loopback) {
      return make_error(ErrorCode::kConfigValidationFailed,
                        "trusted_result requires loopback or an explicit trusted network override");
    }
    if (config.trusted_result.service_token_env.empty()) {
      return make_error(ErrorCode::kConfigValidationFailed,
                        "trusted_result.service_token_env must not be empty");
    }
    if (config.trusted_result.result_wait_timeout_ms < 1'000 ||
        config.trusted_result.result_wait_timeout_ms > 3'600'000) {
      return make_error(ErrorCode::kConfigValidationFailed,
                        "trusted_result.result_wait_timeout_ms must be between 1000 and 3600000");
    }
    if (config.trusted_result.max_request_body_bytes < 1'024 ||
        config.trusted_result.max_request_body_bytes > 65'536) {
      return make_error(ErrorCode::kConfigValidationFailed,
                        "trusted_result.max_request_body_bytes must be between 1024 and 65536");
    }
  } else if (config.trusted_result.allow_non_loopback) {
    return make_error(ErrorCode::kConfigValidationFailed,
                      "trusted_result.allow_non_loopback requires trusted_result.enabled");
  }

  static const std::set<std::string> kLogLevels{"trace", "debug", "info", "warn", "error", "critical", "off"};
  if (kLogLevels.find(config.logging.level) == kLogLevels.end()) {
    return make_error(ErrorCode::kConfigValidationFailed, "logging.level is unsupported");
  }
  return {};
}

ConfigResult load_app_config(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    return {std::nullopt, make_error(ErrorCode::kConfigFileNotFound, "configuration file not found")};
  }

  try {
    const auto document = nlohmann::json::parse(input);
    AppConfig config;
    read_if_present(document, "environment", config.environment);

    if (const auto it = document.find("network"); it != document.end()) {
      read_if_present(*it, "bind_address", config.network.bind_address);
      read_if_present(*it, "tcp_port", config.network.tcp_port);
      read_if_present(*it, "observer_port", config.network.observer_port);
      read_if_present(*it, "max_frame_bytes", config.network.max_frame_bytes);
    }
    if (const auto it = document.find("execution"); it != document.end()) {
      read_if_present(*it, "io_threads", config.execution.io_threads);
      read_if_present(*it, "storage_threads", config.execution.storage_threads);
      read_if_present(*it, "storage_queue_capacity", config.execution.storage_queue_capacity);
    }
    if (const auto it = document.find("logging"); it != document.end()) {
      read_if_present(*it, "level", config.logging.level);
      read_if_present(*it, "file", config.logging.file);
    }
    if (const auto it = document.find("session"); it != document.end()) {
      read_if_present(*it, "hello_timeout_ms", config.session.hello_timeout_ms);
      read_if_present(*it, "idle_timeout_ms", config.session.idle_timeout_ms);
      read_if_present(*it, "reconnect_window_ms", config.session.reconnect_window_ms);
    }
    if (const auto it = document.find("data"); it != document.end()) {
      read_if_present(*it, "enabled", config.data.enabled);
      read_if_present(*it, "repair_interval_ms", config.data.repair_interval_ms);
      read_if_present(*it, "repair_batch_size", config.data.repair_batch_size);
      if (const auto mysql = it->find("mysql"); mysql != it->end()) {
        read_if_present(*mysql, "uri", config.data.mysql.uri);
        read_if_present(*mysql, "user", config.data.mysql.user);
        read_if_present(*mysql, "password_env", config.data.mysql.password_env);
        read_if_present(*mysql, "schema", config.data.mysql.schema);
      }
      if (const auto redis = it->find("redis"); redis != it->end()) {
        read_if_present(*redis, "host", config.data.redis.host);
        read_if_present(*redis, "port", config.data.redis.port);
        read_if_present(*redis, "database", config.data.redis.database);
        read_if_present(*redis, "password_env", config.data.redis.password_env);
        read_if_present(*redis, "key_prefix", config.data.redis.key_prefix);
        read_if_present(*redis, "connect_timeout_ms", config.data.redis.connect_timeout_ms);
      }
      if (const auto rules = it->find("rules"); rules != it->end()) {
        read_if_present(*rules, "match_script", config.data.rules.match_script);
        read_if_present(*rules, "score_script", config.data.rules.score_script);
      }
    }
    if (const auto it = document.find("observer"); it != document.end()) {
      read_if_present(*it, "enabled", config.observer.enabled);
      read_if_present(*it, "demo_mode", config.observer.demo_mode);
      read_if_present(*it, "allow_non_loopback_demo", config.observer.allow_non_loopback_demo);
      read_if_present(*it, "static_root", config.observer.static_root);
      read_if_present(*it, "max_events", config.observer.max_events);
    }
    if (const auto it = document.find("trusted_result"); it != document.end()) {
      read_if_present(*it, "enabled", config.trusted_result.enabled);
      read_if_present(*it, "bind_address", config.trusted_result.bind_address);
      read_if_present(*it, "port", config.trusted_result.port);
      read_if_present(*it, "allow_non_loopback", config.trusted_result.allow_non_loopback);
      read_if_present(*it, "service_token_env", config.trusted_result.service_token_env);
      read_if_present(*it, "result_wait_timeout_ms", config.trusted_result.result_wait_timeout_ms);
      read_if_present(*it, "max_request_body_bytes", config.trusted_result.max_request_body_bytes);
    }

    if (auto error = validate_app_config(config)) {
      return {std::nullopt, std::move(error)};
    }
    return {std::move(config), {}};
  } catch (const nlohmann::json::exception&) {
    return {std::nullopt, make_error(ErrorCode::kConfigParseFailed, "configuration JSON is invalid")};
  } catch (const std::exception&) {
    return {std::nullopt, make_error(ErrorCode::kConfigParseFailed, "configuration could not be read")};
  }
}

}  // namespace arena_core
