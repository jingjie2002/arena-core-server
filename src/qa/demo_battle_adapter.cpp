#include "arena_core/qa/demo_battle_adapter.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

namespace arena_core::qa {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

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
  try {
    asio::io_context io_context;
    tcp::resolver resolver(io_context);
    beast::tcp_stream stream(io_context);
    stream.connect(resolver.resolve(config_.host, std::to_string(config_.port)));

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
    http::write(stream, request);

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(stream, buffer, response);
    beast::error_code ignored;
    stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
    if (response.result() != http::status::created && response.result() != http::status::ok) {
      std::string detail;
      const auto value = nlohmann::json::parse(response.body(), nullptr, false);
      if (value.is_object()) {
        detail = value.value("error", std::string{});
      }
      return make_error(ErrorCode::kSettlementStorageFailed,
                        "trusted result ingress rejected demo result: HTTP " +
                            std::to_string(response.result_int()) +
                            (detail.empty() ? std::string{} : " " + detail));
    }
    return {};
  } catch (const std::exception& error) {
    return make_error(ErrorCode::kSettlementStorageFailed,
                      std::string("trusted result ingress is unavailable: ") + error.what());
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
