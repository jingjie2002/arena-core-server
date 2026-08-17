#include "arena_core/qa/lab_http_server.hpp"

#include "arena_core/qa/lab_run.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace arena_core::qa {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

std::filesystem::path unique_test_directory() {
  static std::atomic<std::uint64_t> sequence{0};
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("arena-lab-http-" + std::to_string(stamp) + "-" +
          std::to_string(sequence.fetch_add(1)));
}

class LabHttpHarness {
 public:
  explicit LabHttpHarness(const bool blocking_handler)
      : root_(unique_test_directory()), reports_(root_ / "reports") {
    std::filesystem::create_directories(root_ / "static");
    write_asset("overview.html", "<!doctype html><title>lab-overview-test</title>");
    write_asset("scenarios.html", "<!doctype html><title>scenarios</title>");
    write_asset("runs.html", "<!doctype html><title>runs</title>");
    write_asset("integration.html", "<!doctype html><title>integration</title>");
    write_asset("styles.css", "body { color: white; }");
    write_asset("app.js", "window.arenaLab = true;");
    write_asset("favicon.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"/>");

    LabRunEnvironment environment;
    environment.git_commit = "http-test";
    environment.service_config_digest = "http-test-config";

    LabScenarioHandler handler = [blocking_handler](const LabScenarioRequest&,
                                                     const LabRunEnvironment&,
                                                     const LabScenarioExecution& execution) {
      execution.reporter->event("test.started", "test handler started");
      if (blocking_handler) {
        const auto end = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < end) {
          if (execution.cancellation->wait_for(10ms)) {
            return make_error(ErrorCode::kQaRunCancelled, "cancelled by HTTP test");
          }
        }
      }
      execution.reporter->assertion("http-test", "handler completed", true, true, true);
      return Error{};
    };
    std::map<LabScenarioKind, LabScenarioHandler> handlers;
    for (const auto kind : {LabScenarioKind::kCompleteMatch,
                            LabScenarioKind::kReconnectRecovery,
                            LabScenarioKind::kDuplicatePlayerRequest,
                            LabScenarioKind::kDuplicateTrustedResult,
                            LabScenarioKind::kRedisFailureRecovery}) {
      handlers.emplace(kind, handler);
    }

    engine = std::make_shared<LabRunEngine>(environment, reports_, std::move(handlers));
    if (const auto error = engine->start()) throw std::runtime_error(error.message);

    LabHttpConfig config;
    config.port = 0;
    config.static_root = root_ / "static";
    server = std::make_shared<LabHttpServer>(io, std::move(config), engine, environment, false);
    if (const auto error = server->start()) throw std::runtime_error(error.message);
    port = server->bound_port();
    thread = std::thread([this] { io.run(); });
  }

  ~LabHttpHarness() {
    server->stop();
    io.stop();
    if (thread.joinable()) thread.join();
    engine->stop();
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  http::response<http::string_body> request(const http::verb method,
                                            const std::string& target,
                                            std::string body = {},
                                            const bool write_header = false,
                                            const bool json_content_type = false) const {
    asio::io_context client_io;
    tcp::socket socket(client_io);
    socket.connect({asio::ip::make_address("127.0.0.1"), port});
    http::request<http::string_body> value{method, target, 11};
    value.set(http::field::host, "127.0.0.1");
    if (write_header) value.set("X-Arena-Lab", "1");
    if (json_content_type) value.set(http::field::content_type, "application/json; charset=utf-8");
    value.body() = std::move(body);
    value.prepare_payload();
    http::write(socket, value);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(socket, buffer, response);
    return response;
  }

  asio::io_context io;
  std::shared_ptr<LabRunEngine> engine;
  std::shared_ptr<LabHttpServer> server;
  std::uint16_t port{0};
  std::thread thread;

 private:
  void write_asset(const std::string& name, const std::string& content) {
    std::ofstream output(root_ / "static" / name, std::ios::binary);
    output << content;
  }

  std::filesystem::path root_;
  std::filesystem::path reports_;
};

nlohmann::json valid_run_request() {
  return {{"schema_version", 1},
          {"scenario", "complete_match"},
          {"seed", 42},
          {"timeout_ms", 10'000},
          {"parameters", {{"player_count", 2}}}};
}

TEST(LabHttpServerTest, ServesAllowlistedStaticAssetsAndCapabilities) {
  LabHttpHarness harness(false);
  const auto page = harness.request(http::verb::get, "/");
  ASSERT_EQ(page.result(), http::status::ok);
  EXPECT_NE(page.body().find("lab-overview-test"), std::string::npos);
  EXPECT_EQ(page[http::field::cache_control], "no-store");
  EXPECT_NE(page["Content-Security-Policy"].find("default-src 'self'"),
            beast::string_view::npos);
  EXPECT_EQ(page["X-Arena-Lab"], "1");

  EXPECT_EQ(harness.request(http::verb::get, "/app.js").result(), http::status::ok);
  const auto favicon = harness.request(http::verb::get, "/favicon.svg");
  EXPECT_EQ(favicon.result(), http::status::ok);
  EXPECT_EQ(favicon[http::field::content_type], "image/svg+xml");
  EXPECT_EQ(harness.request(http::verb::get, "/../CMakeLists.txt").result(),
            http::status::not_found);
  EXPECT_EQ(harness.request(http::verb::get, "/not-allowlisted.txt").result(),
            http::status::not_found);

  const auto capabilities_response =
      harness.request(http::verb::get, "/api/lab/v1/capabilities");
  ASSERT_EQ(capabilities_response.result(), http::status::ok);
  const auto capabilities = nlohmann::json::parse(capabilities_response.body());
  EXPECT_EQ(capabilities.at("service").at("name"), "arena-core-lab");
  ASSERT_EQ(capabilities.at("scenarios").size(), 5U);
  EXPECT_EQ(capabilities.at("scenarios").at(0).at("schema_url"),
            "/api/lab/v1/scenarios/complete_match/schema");

  const auto health = nlohmann::json::parse(
      harness.request(http::verb::get, "/api/lab/v1/health").body());
  EXPECT_EQ(health.at("status"), "ok");
  EXPECT_FALSE(health.at("active_run").get<bool>());
  EXPECT_EQ(health.at("schema_version"), 1);

  const auto schema = harness.request(
      http::verb::get, "/api/lab/v1/scenarios/complete_match/schema");
  ASSERT_EQ(schema.result(), http::status::ok);
  EXPECT_EQ(nlohmann::json::parse(schema.body()).at("name"), "complete_match");
}

TEST(LabHttpServerTest, RejectsUnsafeOrInvalidWriteRequests) {
  LabHttpHarness harness(false);
  const auto body = valid_run_request().dump();
  EXPECT_EQ(harness.request(http::verb::post, "/api/lab/v1/runs", body, false, true).result(),
            http::status::forbidden);
  EXPECT_EQ(harness.request(http::verb::post, "/api/lab/v1/runs", body, true, false).result(),
            http::status::unsupported_media_type);
  EXPECT_EQ(harness.request(http::verb::post, "/api/lab/v1/runs",
                            std::string(65U * 1024U, 'x'), true, true).result(),
            http::status::payload_too_large);

  auto invalid = valid_run_request();
  invalid["parameters"]["player_count"] = 3;
  const auto rejected = harness.request(http::verb::post, "/api/lab/v1/runs", invalid.dump(),
                                        true, true);
  ASSERT_EQ(rejected.result(), http::status::bad_request);
  EXPECT_EQ(nlohmann::json::parse(rejected.body()).at("error"), "QA_SCENARIO_INVALID");

  const nlohmann::json unavailable{{"schema_version", 1},
                                   {"scenario", "redis_failure_recovery"},
                                   {"parameters", nlohmann::json::object()}};
  EXPECT_EQ(harness.request(http::verb::post, "/api/lab/v1/runs", unavailable.dump(),
                            true, true).result(),
            http::status::service_unavailable);
}

TEST(LabHttpServerTest, CreatesReadsStreamsReplaysAndCancelsRun) {
  LabHttpHarness harness(true);
  const auto created_response = harness.request(http::verb::post, "/api/lab/v1/runs",
                                                valid_run_request().dump(), true, true);
  ASSERT_EQ(created_response.result(), http::status::accepted);
  const auto created = nlohmann::json::parse(created_response.body());
  const auto run_id = created.at("run_id").get<std::string>();
  ASSERT_FALSE(run_id.empty());

  const auto current = harness.request(http::verb::get, "/api/lab/v1/runs/" + run_id);
  ASSERT_EQ(current.result(), http::status::ok);
  EXPECT_EQ(nlohmann::json::parse(current.body()).at("run_id"), run_id);

  const auto events = harness.request(http::verb::get,
                                      "/api/lab/v1/runs/" + run_id + "/events?after_seq=0");
  ASSERT_EQ(events.result(), http::status::ok);
  EXPECT_EQ(nlohmann::json::parse(events.body()).at("run_id"), run_id);
  EXPECT_EQ(harness.request(http::verb::get,
                            "/api/lab/v1/runs/" + run_id + "/events?after_seq=bad").result(),
            http::status::bad_request);

  EXPECT_EQ(harness.request(http::verb::post, "/api/lab/v1/runs/" + run_id + "/replay",
                            "{}", true, true).result(),
            http::status::conflict);
  EXPECT_EQ(harness.request(http::verb::post, "/api/lab/v1/runs/" + run_id + "/cancel",
                            "{}", true, true).result(),
            http::status::accepted);

  nlohmann::json terminal;
  for (int attempt = 0; attempt < 100; ++attempt) {
    const auto response = harness.request(http::verb::get, "/api/lab/v1/runs/" + run_id);
    ASSERT_EQ(response.result(), http::status::ok);
    terminal = nlohmann::json::parse(response.body());
    if (terminal.at("status") == "Cancelled") break;
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_EQ(terminal.at("status"), "Cancelled");

  const auto replay_response = harness.request(http::verb::post,
                                               "/api/lab/v1/runs/" + run_id + "/replay",
                                               "{}", true, true);
  ASSERT_EQ(replay_response.result(), http::status::accepted);
  const auto replay = nlohmann::json::parse(replay_response.body());
  EXPECT_EQ(replay.at("replay_of"), run_id);
  EXPECT_NE(replay.at("run_id"), run_id);

  EXPECT_EQ(harness.request(http::verb::post,
                            "/api/lab/v1/runs/does-not-exist/cancel", "{}", true, true).result(),
            http::status::not_found);
}

}  // namespace
}  // namespace arena_core::qa
