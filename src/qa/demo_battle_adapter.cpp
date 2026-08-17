#include "arena_core/qa/demo_battle_adapter.hpp"

#include "bounded_http.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

namespace arena_core::qa {
namespace {

namespace http = boost::beast::http;

Error validate_config(const DemoBattleAdapterConfig& config) {
  if (config.host.empty() || config.port == 0U || config.service_token.empty() ||
      config.request_timeout_ms == 0U || config.request_timeout_ms > 30'000U) {
    return make_error(ErrorCode::kConfigValidationFailed,
                      "trusted result adapter configuration is invalid");
  }
  return {};
}

Error response_error(const http::response<http::string_body>& response) {
  const auto value = nlohmann::json::parse(response.body(), nullptr, false);
  const auto detail = value.is_object()
      ? value.value("message", value.value("error", std::string{})) : std::string{};
  ErrorCode code = ErrorCode::kSettlementStorageFailed;
  if (response.result() == http::status::conflict) code = ErrorCode::kSettlementConflict;
  else if (response.result() == http::status::not_found) code = ErrorCode::kSettlementNotFound;
  else if (response.result() == http::status::unauthorized ||
           response.result() == http::status::forbidden) code = ErrorCode::kSettlementAccessDenied;
  else if (response.result() == http::status::unprocessable_entity ||
           response.result() == http::status::bad_request) code = ErrorCode::kSettlementNotAllowed;
  return make_error(code, "trusted result ingress returned HTTP " +
      std::to_string(response.result_int()) +
      (detail.empty() ? std::string{} : ": " + detail));
}

Result<DemoBattleReceipt> parse_receipt(const http::response<http::string_body>& response) {
  if (response.result() != http::status::created && response.result() != http::status::ok) {
    return Result<DemoBattleReceipt>::failure(response_error(response));
  }
  const auto value = nlohmann::json::parse(response.body(), nullptr, false);
  if (!value.is_object()) {
    return Result<DemoBattleReceipt>::failure(
        make_error(ErrorCode::kProtocolDecodeFailed, "trusted result receipt JSON is invalid"));
  }
  DemoBattleReceipt receipt;
  receipt.http_status = static_cast<std::uint16_t>(response.result_int());
  receipt.status = value.value("status", std::string{});
  receipt.result_id = value.value("result_id", std::string{});
  receipt.match_id = value.value("match_id", std::string{});
  receipt.room_id = value.value("room_id", std::string{});
  receipt.winner_id = value.value("winner_id", std::string{});
  receipt.loser_id = value.value("loser_id", std::string{});
  receipt.cache_state = value.value("cache_state", std::string{});
  receipt.idempotent = value.value("idempotent", false);
  receipt.body = value;
  return Result<DemoBattleReceipt>::success(std::move(receipt));
}

bool safe_result_id(const std::string& value) {
  return !value.empty() && value.size() <= 64U &&
      std::all_of(value.begin(), value.end(), [](const char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-' || character == '_';
      });
}

std::tm utc_time(const std::time_t value) {
  std::tm result{};
#if defined(_WIN32)
  gmtime_s(&result, &value);
#else
  gmtime_r(&value, &result);
#endif
  return result;
}

}  // namespace

DemoBattleAdapter::DemoBattleAdapter(DemoBattleAdapterConfig config)
    : config_(std::move(config)) {}

Error DemoBattleAdapter::submit(const DemoBattleResult& result) const {
  const auto receipt = submit_with_receipt(result);
  return receipt.ok() ? Error{} : receipt.error;
}

Result<DemoBattleReceipt> DemoBattleAdapter::submit_with_receipt(
    const DemoBattleResult& result) const {
  if (const auto error = validate_config(config_)) {
    return Result<DemoBattleReceipt>::failure(error);
  }
  try {
    http::request<http::string_body> request{http::verb::post,
                                             "/internal/v1/match-results", 11};
    request.set(http::field::host, config_.host);
    request.set(http::field::user_agent, "arena-core-demo-battle-adapter/1");
    request.set(http::field::content_type, "application/json");
    request.set(http::field::authorization, "Bearer " + config_.service_token);
    request.body() = nlohmann::json{{"result_id", result.result_id},
                                    {"match_id", result.match_id},
                                    {"room_id", result.room_id},
                                    {"winner_id", result.winner_id},
                                    {"loser_id", result.loser_id},
                                    {"rule_version", result.rule_version},
                                    {"finished_at", result.finished_at}}
                         .dump();
    request.prepare_payload();
    request.keep_alive(false);
    auto exchange = detail::bounded_http_request(
        config_.host, config_.port, std::move(request),
        std::chrono::milliseconds(config_.request_timeout_ms));
    if (!exchange.response.has_value()) {
      return Result<DemoBattleReceipt>::failure(make_error(
          ErrorCode::kSettlementStorageFailed,
          "trusted result ingress is unavailable: " + exchange.error));
    }
    return parse_receipt(*exchange.response);
  } catch (const std::exception& error) {
    return Result<DemoBattleReceipt>::failure(make_error(
        ErrorCode::kSettlementStorageFailed,
        std::string("trusted result ingress is unavailable: ") + error.what()));
  }
}

Result<DemoBattleReceipt> DemoBattleAdapter::query(const std::string& result_id) const {
  if (!safe_result_id(result_id)) {
    return Result<DemoBattleReceipt>::failure(
        make_error(ErrorCode::kSettlementNotFound, "trusted result id is invalid"));
  }
  if (const auto error = validate_config(config_)) {
    return Result<DemoBattleReceipt>::failure(error);
  }
  try {
    http::request<http::string_body> request{
        http::verb::get, "/internal/v1/match-results/" + result_id, 11};
    request.set(http::field::host, config_.host);
    request.set(http::field::user_agent, "arena-core-lab/1");
    request.set(http::field::authorization, "Bearer " + config_.service_token);
    request.keep_alive(false);
    auto exchange = detail::bounded_http_request(
        config_.host, config_.port, std::move(request),
        std::chrono::milliseconds(config_.request_timeout_ms));
    if (!exchange.response.has_value()) {
      return Result<DemoBattleReceipt>::failure(make_error(
          ErrorCode::kSettlementStorageFailed,
          "trusted result ingress is unavailable: " + exchange.error));
    }
    return parse_receipt(*exchange.response);
  } catch (const std::exception& error) {
    return Result<DemoBattleReceipt>::failure(make_error(
        ErrorCode::kSettlementStorageFailed,
        std::string("trusted result ingress is unavailable: ") + error.what()));
  }
}

std::string current_utc_rfc3339() {
  const auto now = std::chrono::system_clock::now();
  const auto value = std::chrono::system_clock::to_time_t(now);
  const auto utc = utc_time(value);
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return output.str();
}

}  // namespace arena_core::qa
