#include "arena_core/observer/http_server.hpp"
#include "arena_core/observer/observer_store.hpp"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace arena_core::observer {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

class HttpHarness {
 public:
  explicit HttpHarness(const bool demo_mode)
      : store(std::make_shared<ObserverStore>()) {
    ObserverHttpConfig config;
    config.port = 0;
    config.demo_mode = demo_mode;
    config.static_root = std::filesystem::path(ARENA_SOURCE_DIR) / "web" / "observer";
    server = std::make_shared<ObserverHttpServer>(io, std::move(config), store);
    const auto error = server->start();
    if (error) {
      throw std::runtime_error(error.message);
    }
    port = server->bound_port();
    thread = std::thread([this] { io.run(); });
  }

  ~HttpHarness() {
    server->stop();
    io.stop();
    if (thread.joinable()) {
      thread.join();
    }
  }

  http::response<http::string_body> request(const http::verb method,
                                            const std::string& target,
                                            const bool demo_header = false) const {
    asio::io_context client_io;
    tcp::socket socket(client_io);
    socket.connect({asio::ip::make_address("127.0.0.1"), port});
    http::request<http::string_body> request{method, target, 11};
    request.set(http::field::host, "127.0.0.1");
    if (demo_header) {
      request.set("X-Arena-Demo", "1");
    }
    request.prepare_payload();
    http::write(socket, request);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(socket, buffer, response);
    return response;
  }

  asio::io_context io;
  std::shared_ptr<ObserverStore> store;
  std::shared_ptr<ObserverHttpServer> server;
  std::uint16_t port{0};
  std::thread thread;
};

TEST(ObserverHttpServerTest, ServesStaticDashboardAndReadOnlyApi) {
  HttpHarness harness(false);
  const auto page = harness.request(http::verb::get, "/");
  EXPECT_EQ(page.result(), http::status::ok);
  EXPECT_NE(page.body().find("竞域"), std::string::npos);
  EXPECT_NE(page.body().find("这不是游戏画面"), std::string::npos);
  EXPECT_NE(page.body().find("只读服务观察台"), std::string::npos);
  EXPECT_NE(page.body().find("不提供写操作"), std::string::npos);

  const auto script = harness.request(http::verb::get, "/app.js");
  EXPECT_EQ(script.result(), http::status::ok);
  EXPECT_NE(script.body().find("重复提交已识别"), std::string::npos);

  const auto favicon = harness.request(http::verb::get, "/favicon.svg");
  EXPECT_EQ(favicon.result(), http::status::ok);
  EXPECT_EQ(favicon[http::field::content_type], "image/svg+xml");

  const auto overview = harness.request(http::verb::get, "/api/overview");
  ASSERT_EQ(overview.result(), http::status::ok);
  EXPECT_EQ(nlohmann::json::parse(overview.body()).at("service"), "arena-core-server");

  const auto rejected = harness.request(http::verb::post, "/api/demo/scenarios/eight-player-flow");
  EXPECT_EQ(rejected.result(), http::status::forbidden);
  const auto traversal = harness.request(http::verb::get, "/../CMakeLists.txt");
  EXPECT_EQ(traversal.result(), http::status::not_found);
}

TEST(ObserverHttpServerTest, DemoModeRunsScenarioAndTraceFilterPreservesOrder) {
  HttpHarness harness(true);
  const auto missing_header = harness.request(http::verb::post, "/api/demo/scenarios/eight-player-flow");
  EXPECT_EQ(missing_header.result(), http::status::forbidden);
  const auto started = harness.request(http::verb::post, "/api/demo/scenarios/eight-player-flow", true);
  EXPECT_EQ(started.result(), http::status::accepted);

  const auto rooms = nlohmann::json::parse(harness.request(http::verb::get, "/api/rooms").body());
  EXPECT_EQ(rooms.at("total"), 4);
  const auto events = nlohmann::json::parse(
      harness.request(http::verb::get, "/api/events?trace_id=trace-demo-1&limit=100").body());
  ASSERT_GE(events.at("items").size(), 4U);
  std::uint64_t previous = 0;
  for (const auto& event : events.at("items")) {
    const auto sequence = event.at("sequence").get<std::uint64_t>();
    EXPECT_GT(sequence, previous);
    EXPECT_EQ(event.at("trace_id"), "trace-demo-1");
    previous = sequence;
  }
}

}  // namespace
}  // namespace arena_core::observer
