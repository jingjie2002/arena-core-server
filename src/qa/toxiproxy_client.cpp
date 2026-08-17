#include "arena_core/qa/toxiproxy_client.hpp"

#include "bounded_http.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

namespace arena_core::qa {
namespace {

namespace http = boost::beast::http;

constexpr std::uint32_t kMaximumFaultValueMs = 60'000;
constexpr std::uint64_t kMaximumResponseBytes = 64U * 1'024U;

bool is_safe_host_character(const unsigned char character) {
  return std::isalnum(character) != 0 || character == '.' || character == '-' ||
         character == '_' || character == ':';
}

bool is_safe_proxy_character(const unsigned char character) {
  return std::isalnum(character) != 0 || character == '.' || character == '-' ||
         character == '_';
}

bool has_only_safe_characters(const std::string_view value,
                              bool (*predicate)(unsigned char)) {
  return std::all_of(value.begin(), value.end(), [predicate](const char character) {
    return predicate(static_cast<unsigned char>(character));
  });
}

bool is_safe_endpoint_value(const std::string_view value) {
  return !value.empty() && value.size() <= 255U &&
         std::none_of(value.begin(), value.end(), [](const char character) {
           const auto byte = static_cast<unsigned char>(character);
           return std::iscntrl(byte) != 0;
         });
}

Error validate_config(const ToxiproxyClientConfig& config) {
  if (config.host.empty() || config.host.size() > 253U ||
      !has_only_safe_characters(config.host, is_safe_host_character)) {
    return make_error(ErrorCode::kConfigValidationFailed,
                      "Toxiproxy host must be a fixed host name or IP address");
  }
  if (config.api_port == 0U) {
    return make_error(ErrorCode::kConfigValidationFailed,
                      "Toxiproxy API port must be non-zero");
  }
  if (config.proxy_name.empty() || config.proxy_name.size() > 128U ||
      !has_only_safe_characters(config.proxy_name, is_safe_proxy_character)) {
    return make_error(ErrorCode::kConfigValidationFailed,
                      "Toxiproxy proxy name contains unsupported characters");
  }
  if (!is_safe_endpoint_value(config.listen) || !is_safe_endpoint_value(config.upstream)) {
    return make_error(ErrorCode::kConfigValidationFailed,
                      "Toxiproxy listen and upstream endpoints must be fixed values");
  }
  if (config.request_timeout_ms < 100U || config.request_timeout_ms > 30'000U) {
    return make_error(ErrorCode::kConfigValidationFailed,
                      "Toxiproxy request timeout must be between 100 and 30000 ms");
  }
  return {};
}

Error request(const ToxiproxyClientConfig& config,
              const http::verb method,
              const std::string_view target,
              const std::string& body = {}) {
  if (const auto error = validate_config(config)) {
    return error;
  }

  try {
    const auto timeout = std::chrono::milliseconds(config.request_timeout_ms);

    http::request<http::string_body> value{method, std::string(target), 11};
    value.set(http::field::host, config.host + ":" + std::to_string(config.api_port));
    value.set(http::field::user_agent, "arena-core-lab-toxiproxy-client/1");
    value.set(http::field::accept, "application/json");
    value.keep_alive(false);
    if (!body.empty()) {
      value.set(http::field::content_type, "application/json");
      value.body() = body;
      value.prepare_payload();
    }
    auto exchange = detail::bounded_http_request(
        config.host, config.api_port, std::move(value), timeout,
        kMaximumResponseBytes);
    if (!exchange.response.has_value()) {
      return make_error(ErrorCode::kDependencyRedisUnavailable,
                        "Toxiproxy API request failed");
    }
    const auto& response = *exchange.response;
    if (response.result_int() < 200 || response.result_int() >= 300) {
      return make_error(ErrorCode::kDependencyRedisUnavailable,
                        "Toxiproxy API rejected the request with HTTP status " +
                            std::to_string(response.result_int()));
    }
    return {};
  } catch (const std::exception&) {
    return make_error(ErrorCode::kDependencyRedisUnavailable,
                      "Toxiproxy API request failed");
  }
}

std::string proxy_target(const ToxiproxyClientConfig& config) {
  return "/proxies/" + config.proxy_name;
}

nlohmann::json toxic_document(const std::string_view type,
                              const std::string_view attribute,
                              const std::uint32_t value_ms) {
  return {{"name", "arena-core-lab-" + std::string(type)},
          {"type", type},
          {"stream", "downstream"},
          {"toxicity", 1.0},
          {"attributes", {{attribute, value_ms}}}};
}

}  // namespace

ToxiproxyClient::ToxiproxyClient(ToxiproxyClientConfig config)
    : config_(std::move(config)) {}

Error ToxiproxyClient::probe() const {
  return request(config_, http::verb::get, "/version");
}

Error ToxiproxyClient::prepare() const {
  if (const auto error = reset()) {
    return error;
  }
  const auto body = nlohmann::json::array(
      {{{"name", config_.proxy_name},
        {"listen", config_.listen},
        {"upstream", config_.upstream},
        {"enabled", true}}});
  return request(config_, http::verb::post, "/populate", body.dump());
}

Error ToxiproxyClient::inject(const ToxiproxyFaultKind kind,
                             const std::uint32_t value_ms) const {
  if (value_ms > kMaximumFaultValueMs) {
    return make_error(ErrorCode::kConfigValidationFailed,
                      "Toxiproxy fault value must not exceed 60000 ms");
  }

  if (kind == ToxiproxyFaultKind::kDisconnect) {
    return request(config_, http::verb::post, proxy_target(config_),
                   nlohmann::json{{"enabled", false}}.dump());
  }
  if (kind == ToxiproxyFaultKind::kLatency) {
    if (value_ms == 0U) {
      return make_error(ErrorCode::kConfigValidationFailed,
                        "Toxiproxy latency must be greater than zero");
    }
    return request(config_, http::verb::post, proxy_target(config_) + "/toxics",
                   toxic_document("latency", "latency", value_ms).dump());
  }
  if (kind == ToxiproxyFaultKind::kTimeout) {
    return request(config_, http::verb::post, proxy_target(config_) + "/toxics",
                   toxic_document("timeout", "timeout", value_ms).dump());
  }
  if (kind == ToxiproxyFaultKind::kResetPeer) {
    return request(config_, http::verb::post, proxy_target(config_) + "/toxics",
                   toxic_document("reset_peer", "timeout", value_ms).dump());
  }
  return make_error(ErrorCode::kConfigValidationFailed,
                    "Toxiproxy fault kind is unsupported");
}

Error ToxiproxyClient::reset() const {
  return request(config_, http::verb::post, "/reset");
}

}  // namespace arena_core::qa
