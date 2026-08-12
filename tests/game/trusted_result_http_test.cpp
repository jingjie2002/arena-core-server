#include "arena_core/domain/identifiers.hpp"
#include "arena_core/game/game_service.hpp"
#include "arena_core/game/trusted_result_http_server.hpp"
#include "arena_core/session/token_generator.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <gtest/gtest.h>

namespace arena_core::game {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

class TrustedHttpHarness final {
 public:
  TrustedHttpHarness() {
    game_service = std::make_shared<GameService>(
        io, GameServiceOptions{}, std::make_shared<domain::SequentialIdGenerator>(),
        std::make_shared<session::DeterministicTokenGenerator>());
    game_service->start();
    TrustedResultHttpConfig config;
    config.port = 0;
    config.service_token = "test-service-token";
    server = std::make_shared<TrustedResultHttpServer>(io, std::move(config), game_service);
    if (const auto error = server->start()) {
      throw std::runtime_error(error.message);
    }
    port = server->bound_port();
    thread = std::thread([this] { io.run(); });
  }

  ~TrustedHttpHarness() {
    server->stop();
    game_service->stop();
    io.stop();
    if (thread.joinable()) {
      thread.join();
    }
  }

  http::response<http::string_body> request(http::verb method,
                                            std::string target,
                                            std::string body = {},
                                            std::string token = {}) const {
    asio::io_context client_io;
    tcp::socket socket(client_io);
    socket.connect({asio::ip::address_v4::loopback(), port});
    http::request<http::string_body> value{method, std::move(target), 11};
    value.set(http::field::host, "127.0.0.1");
    if (!token.empty()) {
      value.set(http::field::authorization, "Bearer " + token);
    }
    value.set(http::field::content_type, "application/json");
    value.body() = std::move(body);
    value.prepare_payload();
    http::write(socket, value);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(socket, buffer, response);
    return response;
  }

  asio::io_context io;
  std::shared_ptr<GameService> game_service;
  std::shared_ptr<TrustedResultHttpServer> server;
  std::uint16_t port{0};
  std::thread thread;
};

TEST(TrustedResultHttpServerTest, RequiresBearerAndDoesNotRevealResultExistence) {
  TrustedHttpHarness harness;
  EXPECT_EQ(harness.request(http::verb::get, "/internal/v1/match-results/result-1").result(),
            http::status::unauthorized);
  EXPECT_EQ(harness.request(http::verb::get, "/internal/v1/match-results/result-1", {},
                            "wrong-token").result(),
            http::status::unauthorized);
  EXPECT_EQ(harness.request(http::verb::get, "/internal/v1/match-results/result-1", {},
                            "test-service-token").result(),
            http::status::not_found);
}

TEST(TrustedResultHttpServerTest, RejectsMalformedTrustedSubmissionBeforeBusinessDispatch) {
  TrustedHttpHarness harness;
  const auto malformed = harness.request(http::verb::post, "/internal/v1/match-results",
                                         "{not-json", "test-service-token");
  EXPECT_EQ(malformed.result(), http::status::bad_request);
  EXPECT_NE(malformed.body().find("PROTOCOL_DECODE_FAILED"), std::string::npos);
}

TEST(TrustedResultHttpServerTest, RefusesToStartWithoutServiceCredential) {
  asio::io_context io;
  auto service = std::make_shared<GameService>(
      io, GameServiceOptions{}, std::make_shared<domain::SequentialIdGenerator>(),
      std::make_shared<session::DeterministicTokenGenerator>());
  TrustedResultHttpConfig config;
  config.port = 0;
  auto server = std::make_shared<TrustedResultHttpServer>(io, std::move(config), service);
  EXPECT_EQ(server->start().code, ErrorCode::kConfigValidationFailed);
}

TEST(TrustedResultTest, ValidatesDatabaseSizedIdAndRealUtcCalendarTimestamp) {
  TrustedMatchResult result{"result-1", "match-1", "room-1", "player-1", "player-2",
                            "score-v1", "2024-02-29T23:59:59.123Z"};
  EXPECT_TRUE(freeze_trusted_result(result).ok());

  result.finished_at = "2023-02-29T23:59:59Z";
  EXPECT_FALSE(freeze_trusted_result(result).ok());
  result.finished_at = "2024-02-29T24:00:00Z";
  EXPECT_FALSE(freeze_trusted_result(result).ok());
  result.finished_at = "2024-02-29T23:59:59.Z";
  EXPECT_FALSE(freeze_trusted_result(result).ok());
  result.finished_at = "2024-02-29T23:59:59Z";
  result.result_id = std::string(65U, 'r');
  EXPECT_FALSE(freeze_trusted_result(result).ok());
}

}  // namespace
}  // namespace arena_core::game
