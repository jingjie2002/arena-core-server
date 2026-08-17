#include "arena_core/qa/lab_app_config.hpp"

#include <cstdlib>
#include <fstream>
#include <set>
#include <utility>

#include <nlohmann/json.hpp>

namespace arena_core::qa {
namespace {

template <typename T>
void read_if_present(const nlohmann::json& object, const char* key, T& target) {
  if (const auto found = object.find(key); found != object.end()) found->get_to(target);
}

Error invalid(std::string message) {
  return make_error(ErrorCode::kConfigValidationFailed, std::move(message));
}

bool valid_endpoint(const std::string& host, const std::uint16_t port) {
  return !host.empty() && host.size() <= 253U && port != 0U;
}

bool valid_proxy_endpoint(const std::string& value) {
  if (value.empty() || value.size() > 300U) return false;
  const auto separator = value.rfind(':');
  if (separator == std::string::npos || separator == 0U || separator + 1U >= value.size()) {
    return false;
  }
  return value.find('/') == std::string::npos && value.find('\\') == std::string::npos;
}

}  // namespace

Error validate_lab_app_config(const LabAppConfig& config) {
  if (config.environment.empty() || config.environment.size() > 64U) {
    return invalid("environment length must be between 1 and 64");
  }
  if (!valid_endpoint(config.http.bind_address, config.http.port)) {
    return invalid("http listener is invalid");
  }
  const auto loopback = config.http.bind_address == "127.0.0.1" ||
                        config.http.bind_address == "::1";
  if (!loopback && !config.http.allow_non_loopback) {
    return invalid("Lab HTTP requires loopback or an explicit private-network override");
  }
  if (config.http.static_root.empty() || config.reports.directory.empty()) {
    return invalid("Lab static and report directories must not be empty");
  }
  if (config.http.maximum_request_body_bytes < 1'024U ||
      config.http.maximum_request_body_bytes > 65'536U) {
    return invalid("Lab maximum request body must be between 1024 and 65536 bytes");
  }
  if (!valid_endpoint(config.player_host, config.player_port) ||
      !valid_endpoint(config.trusted_result.host, config.trusted_result.port) ||
      !valid_endpoint(config.observer_host, config.observer_port)) {
    return invalid("Lab target endpoint is invalid");
  }
  if (config.trusted_result.service_token_env.empty() ||
      config.trusted_result.service_token_env.size() > 128U) {
    return invalid("trusted result token environment name is invalid");
  }
  if (config.reconnect_window_ms < 100U || config.reconnect_window_ms > 120'000U) {
    return invalid("reconnect window must be between 100 and 120000 ms");
  }
  if (config.git_commit.empty() || config.git_commit.size() > 64U ||
      config.service_config_digest.empty() || config.service_config_digest.size() > 128U) {
    return invalid("build evidence identifiers are invalid");
  }
  if (config.toxiproxy.enabled) {
    if (!valid_endpoint(config.toxiproxy.host, config.toxiproxy.port) ||
        config.toxiproxy.proxy_name.empty() || config.toxiproxy.proxy_name.size() > 128U ||
        !valid_proxy_endpoint(config.toxiproxy.listen) ||
        !valid_proxy_endpoint(config.toxiproxy.upstream)) {
      return invalid("Toxiproxy endpoint is invalid");
    }
  }
  return {};
}

Result<LabAppConfig> load_lab_app_config(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    return Result<LabAppConfig>::failure(
        make_error(ErrorCode::kConfigFileNotFound, "Lab configuration file not found"));
  }
  try {
    const auto document = nlohmann::json::parse(input);
    LabAppConfig config;
    read_if_present(document, "environment", config.environment);
    read_if_present(document, "git_commit", config.git_commit);
    read_if_present(document, "service_config_digest", config.service_config_digest);
    if (const auto found = document.find("http"); found != document.end()) {
      read_if_present(*found, "bind_address", config.http.bind_address);
      read_if_present(*found, "port", config.http.port);
      read_if_present(*found, "allow_non_loopback", config.http.allow_non_loopback);
      read_if_present(*found, "static_root", config.http.static_root);
      read_if_present(*found, "maximum_request_body_bytes",
                      config.http.maximum_request_body_bytes);
    }
    if (const auto found = document.find("reports"); found != document.end()) {
      read_if_present(*found, "directory", config.reports.directory);
    }
    if (const auto found = document.find("player"); found != document.end()) {
      read_if_present(*found, "host", config.player_host);
      read_if_present(*found, "port", config.player_port);
    }
    if (const auto found = document.find("trusted_result"); found != document.end()) {
      read_if_present(*found, "host", config.trusted_result.host);
      read_if_present(*found, "port", config.trusted_result.port);
      read_if_present(*found, "service_token_env", config.trusted_result.service_token_env);
    }
    if (const auto found = document.find("observer"); found != document.end()) {
      read_if_present(*found, "host", config.observer_host);
      read_if_present(*found, "port", config.observer_port);
    }
    if (const auto found = document.find("session"); found != document.end()) {
      read_if_present(*found, "reconnect_window_ms", config.reconnect_window_ms);
    }
    if (const auto found = document.find("toxiproxy"); found != document.end()) {
      read_if_present(*found, "enabled", config.toxiproxy.enabled);
      read_if_present(*found, "host", config.toxiproxy.host);
      read_if_present(*found, "port", config.toxiproxy.port);
      read_if_present(*found, "proxy_name", config.toxiproxy.proxy_name);
      read_if_present(*found, "listen", config.toxiproxy.listen);
      read_if_present(*found, "upstream", config.toxiproxy.upstream);
    }
    if (const auto error = validate_lab_app_config(config)) {
      return Result<LabAppConfig>::failure(error);
    }
    return Result<LabAppConfig>::success(std::move(config));
  } catch (const nlohmann::json::exception&) {
    return Result<LabAppConfig>::failure(
        make_error(ErrorCode::kConfigParseFailed, "Lab configuration JSON is invalid"));
  } catch (const std::exception&) {
    return Result<LabAppConfig>::failure(
        make_error(ErrorCode::kConfigParseFailed, "Lab configuration could not be read"));
  }
}

Result<LabRunEnvironment> lab_run_environment(const LabAppConfig& config) {
  const auto token_value = std::getenv(config.trusted_result.service_token_env.c_str());
  if (token_value == nullptr || *token_value == '\0') {
    return Result<LabRunEnvironment>::failure(make_error(
        ErrorCode::kConfigValidationFailed,
        "trusted result service token environment variable is missing"));
  }
  LabRunEnvironment environment;
  environment.player_host = config.player_host;
  environment.player_port = config.player_port;
  environment.result_host = config.trusted_result.host;
  environment.result_port = config.trusted_result.port;
  environment.result_token = token_value;
  environment.observer_host = config.observer_host;
  environment.observer_port = config.observer_port;
  environment.reconnect_window_ms = config.reconnect_window_ms;
  environment.git_commit = config.git_commit;
  environment.service_config_digest = config.service_config_digest;
  if (config.toxiproxy.enabled) {
    environment.toxiproxy_host = config.toxiproxy.host;
    environment.toxiproxy_port = config.toxiproxy.port;
    environment.toxiproxy_proxy_name = config.toxiproxy.proxy_name;
    environment.toxiproxy_listen = config.toxiproxy.listen;
    environment.toxiproxy_upstream = config.toxiproxy.upstream;
  }
  return Result<LabRunEnvironment>::success(std::move(environment));
}

}  // namespace arena_core::qa
