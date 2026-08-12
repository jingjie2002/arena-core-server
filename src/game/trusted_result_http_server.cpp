#include "arena_core/game/trusted_result_http_server.hpp"

#include "arena_core/game/game_service.hpp"

#include <string_view>
#include <utility>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

namespace arena_core::game {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

http::response<http::string_body> json_response(const http::status status,
                                                const unsigned version,
                                                nlohmann::json body) {
  http::response<http::string_body> response{status, version};
  response.set(http::field::server, "arena-core-trusted-result/1");
  response.set(http::field::content_type, "application/json; charset=utf-8");
  response.set(http::field::cache_control, "no-store");
  response.set("X-Content-Type-Options", "nosniff");
  response.set("X-Frame-Options", "DENY");
  response.keep_alive(false);
  response.body() = body.dump();
  response.prepare_payload();
  return response;
}

bool constant_time_equal(const std::string_view left, const std::string_view right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  unsigned char difference = 0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    difference = static_cast<unsigned char>(difference |
        static_cast<unsigned char>(left[index] ^ right[index]));
  }
  return difference == 0;
}

bool authorized(const http::request<http::string_body>& request,
                const std::string& service_token) noexcept {
  constexpr std::string_view kPrefix{"Bearer "};
  const auto header = request[http::field::authorization];
  const std::string_view value(header.data(), header.size());
  return value.size() > kPrefix.size() && value.substr(0, kPrefix.size()) == kPrefix &&
         constant_time_equal(value.substr(kPrefix.size()), service_token);
}

nlohmann::json receipt_json(const settlement::SettlementReceipt& receipt) {
  nlohmann::json players = nlohmann::json::array();
  for (const auto& player : receipt.players) {
    players.push_back({{"player_id", player.player_id},
                       {"before_score", player.before_score},
                       {"after_score", player.after_score},
                       {"outcome", player.outcome}});
  }
  return {{"status", receipt.idempotent ? "idempotent_replay" : "accepted"},
          {"result_id", receipt.settlement_id},
          {"match_id", receipt.match_id},
          {"room_id", receipt.room_id},
          {"winner_id", receipt.winner_id},
          {"loser_id", receipt.loser_id},
          {"rule_version", receipt.rule_version},
          {"finished_at", receipt.finished_at},
          {"fingerprint", receipt.fingerprint},
          {"cache_state", receipt.cache_state},
          {"idempotent", receipt.idempotent},
          {"players", std::move(players)}};
}

http::status status_for_error(const ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kSettlementNotFound:
    case ErrorCode::kRoomNotFound:
      return http::status::not_found;
    case ErrorCode::kSettlementConflict:
      return http::status::conflict;
    case ErrorCode::kSettlementNotAllowed:
    case ErrorCode::kSettlementAccessDenied:
    case ErrorCode::kSettlementPlayerSetInvalid:
    case ErrorCode::kRuleVersionMismatch:
    case ErrorCode::kRoomInvalidState:
      return http::status::unprocessable_entity;
    case ErrorCode::kSettlementStorageFailed:
    case ErrorCode::kDependencyMysqlUnavailable:
    case ErrorCode::kDependencyRedisUnavailable:
    case ErrorCode::kServerBusy:
    case ErrorCode::kServerShuttingDown:
      return http::status::service_unavailable;
    default:
      return http::status::internal_server_error;
  }
}

nlohmann::json error_json(const Error& error) {
  return {{"error", std::string(error_code_name(error.code))},
          {"message", error.message},
          {"retryable", error.retryable}};
}

Result<TrustedMatchResult> parse_submission(const std::string& body) {
  try {
    const auto value = nlohmann::json::parse(body);
    TrustedMatchResult result;
    result.result_id = value.at("result_id").get<std::string>();
    result.match_id = value.at("match_id").get<std::string>();
    result.room_id = value.at("room_id").get<std::string>();
    result.winner_id = value.at("winner_id").get<std::string>();
    result.loser_id = value.at("loser_id").get<std::string>();
    result.rule_version = value.at("rule_version").get<std::string>();
    result.finished_at = value.at("finished_at").get<std::string>();
    return Result<TrustedMatchResult>::success(std::move(result));
  } catch (const nlohmann::json::exception&) {
    return Result<TrustedMatchResult>::failure(
        make_error(ErrorCode::kProtocolDecodeFailed, "trusted result JSON is invalid"));
  }
}

class HttpSession final : public std::enable_shared_from_this<HttpSession> {
 public:
  HttpSession(tcp::socket socket,
              TrustedResultHttpConfig config,
              std::shared_ptr<GameService> game_service)
      : socket_(std::move(socket)), config_(std::move(config)),
        game_service_(std::move(game_service)) {
    parser_.body_limit(config_.maximum_request_body_bytes);
  }

  void start() {
    http::async_read(socket_, buffer_, parser_,
                     [self = shared_from_this()](const beast::error_code& error, std::size_t) {
                       if (error) {
                         self->close();
                         return;
                       }
                       self->route();
                     });
  }

 private:
  void route() {
    const auto& request = parser_.get();
    if (!authorized(request, config_.service_token)) {
      auto response = json_response(http::status::unauthorized, request.version(),
                                    {{"error", "unauthorized"}});
      response.set(http::field::www_authenticate, "Bearer");
      write(std::move(response));
      return;
    }
    constexpr std::string_view kCollection{"/internal/v1/match-results"};
    const std::string target(request.target().data(), request.target().size());
    if (request.method() == http::verb::post && target == std::string(kCollection)) {
      auto parsed = parse_submission(request.body());
      if (!parsed.ok()) {
        write(json_response(http::status::bad_request, request.version(), error_json(parsed.error)));
        return;
      }
      const auto version = request.version();
      game_service_->async_submit_trusted_result(
          std::move(*parsed.value),
          [self = shared_from_this(), version](Result<settlement::SettlementReceipt> result) mutable {
            asio::dispatch(self->socket_.get_executor(),
                           [self, version, result = std::move(result)]() mutable {
              if (!result.ok()) {
                self->write(json_response(status_for_error(result.error.code), version,
                                          error_json(result.error)));
                return;
              }
              const auto status = result.value->idempotent ? http::status::ok : http::status::created;
              self->write(json_response(status, version, receipt_json(*result.value)));
            });
          });
      return;
    }
    const auto item_prefix = std::string(kCollection) + "/";
    if (request.method() == http::verb::get && target.compare(0, item_prefix.size(), item_prefix) == 0) {
      const auto result_id = target.substr(item_prefix.size());
      const auto version = request.version();
      game_service_->async_query_trusted_result(
          settlement::SettlementQuery{result_id, ""},
          [self = shared_from_this(), version](Result<settlement::SettlementReceipt> result) mutable {
            asio::dispatch(self->socket_.get_executor(),
                           [self, version, result = std::move(result)]() mutable {
              if (!result.ok()) {
                self->write(json_response(status_for_error(result.error.code), version,
                                          error_json(result.error)));
                return;
              }
              self->write(json_response(http::status::ok, version, receipt_json(*result.value)));
            });
          });
      return;
    }
    write(json_response(http::status::not_found, request.version(), {{"error", "not_found"}}));
  }

  void write(http::response<http::string_body> response) {
    response_ = std::make_shared<http::response<http::string_body>>(std::move(response));
    http::async_write(socket_, *response_,
                      [self = shared_from_this()](const beast::error_code&, std::size_t) {
                        self->close();
                      });
  }

  void close() {
    beast::error_code ignored;
    socket_.shutdown(tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
  }

  tcp::socket socket_;
  beast::flat_buffer buffer_;
  http::request_parser<http::string_body> parser_;
  TrustedResultHttpConfig config_;
  std::shared_ptr<GameService> game_service_;
  std::shared_ptr<http::response<http::string_body>> response_;
};

}  // namespace

TrustedResultHttpServer::TrustedResultHttpServer(asio::io_context& io_context,
                                                 TrustedResultHttpConfig config,
                                                 std::shared_ptr<GameService> game_service)
    : strand_(asio::make_strand(io_context)), acceptor_(strand_), config_(std::move(config)),
      game_service_(std::move(game_service)) {}

Error TrustedResultHttpServer::start() {
  if (started_) {
    return {};
  }
  if (config_.service_token.empty() || config_.service_token.size() > 512U) {
    return make_error(ErrorCode::kConfigValidationFailed,
                      "trusted result service token is missing or too long");
  }
  boost::system::error_code error;
  const auto address = asio::ip::make_address(config_.bind_address, error);
  if (error) {
    return make_error(ErrorCode::kConfigValidationFailed,
                      "trusted result bind address is invalid");
  }
  const tcp::endpoint endpoint(address, config_.port);
  acceptor_.open(endpoint.protocol(), error);
  if (error) {
    return make_error(ErrorCode::kInternalError, "trusted result acceptor open failed");
  }
  acceptor_.set_option(tcp::acceptor::reuse_address(true), error);
  acceptor_.bind(endpoint, error);
  if (error) {
    return make_error(ErrorCode::kInternalError, "trusted result bind failed");
  }
  acceptor_.listen(asio::socket_base::max_listen_connections, error);
  if (error) {
    return make_error(ErrorCode::kInternalError, "trusted result listen failed");
  }
  started_ = true;
  stopped_ = false;
  do_accept();
  return {};
}

void TrustedResultHttpServer::stop() {
  asio::dispatch(strand_, [self = shared_from_this()] {
    if (self->stopped_) {
      return;
    }
    self->stopped_ = true;
    boost::system::error_code ignored;
    self->acceptor_.cancel(ignored);
    self->acceptor_.close(ignored);
  });
}

std::uint16_t TrustedResultHttpServer::bound_port() const {
  boost::system::error_code error;
  const auto endpoint = acceptor_.local_endpoint(error);
  return error ? 0 : endpoint.port();
}

void TrustedResultHttpServer::do_accept() {
  acceptor_.async_accept(asio::bind_executor(
      strand_, [self = shared_from_this()](const boost::system::error_code& error, tcp::socket socket) {
        if (!error && !self->stopped_) {
          std::make_shared<HttpSession>(std::move(socket), self->config_, self->game_service_)->start();
        }
        if (!self->stopped_) {
          self->do_accept();
        }
      }));
}

}  // namespace arena_core::game
