#include "arena_core/qa/toxiproxy_client.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

struct RecordedRequest {
  http::verb method{http::verb::unknown};
  std::string target;
  std::string body;
};

struct StubResponse {
  http::status status{http::status::ok};
  std::string body{"{}"};
  std::uint32_t delay_ms{0U};
};

class ToxiproxyApiStub final {
 public:
  explicit ToxiproxyApiStub(std::vector<StubResponse> responses)
      : acceptor_(io_, {asio::ip::address_v4::loopback(), 0}),
        responses_(std::move(responses)),
        thread_([this] { serve(); }) {}

  ~ToxiproxyApiStub() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  ToxiproxyApiStub(const ToxiproxyApiStub&) = delete;
  ToxiproxyApiStub& operator=(const ToxiproxyApiStub&) = delete;

  std::uint16_t port() const {
    return acceptor_.local_endpoint().port();
  }

  void wait() {
    if (thread_.joinable()) {
      thread_.join();
    }
    if (!failure_.empty()) {
      throw std::runtime_error(failure_);
    }
  }

  std::vector<RecordedRequest> requests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_;
  }

 private:
  void serve() noexcept {
    try {
      for (const auto& configured : responses_) {
        tcp::socket socket(io_);
        acceptor_.accept(socket);
        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        http::read(socket, buffer, request);
        {
          std::lock_guard<std::mutex> lock(mutex_);
          requests_.push_back(
              {request.method(), std::string(request.target()), request.body()});
        }

        http::response<http::string_body> response{configured.status, 11};
        response.set(http::field::content_type, "application/json");
        response.keep_alive(false);
        response.body() = configured.body;
        response.prepare_payload();
        if (configured.delay_ms > 0U) {
          std::this_thread::sleep_for(std::chrono::milliseconds(configured.delay_ms));
        }
        beast::error_code ignored;
        http::write(socket, response, ignored);
        socket.shutdown(tcp::socket::shutdown_both, ignored);
      }
    } catch (const std::exception& error) {
      std::lock_guard<std::mutex> lock(mutex_);
      failure_ = error.what();
    }
  }

  asio::io_context io_;
  tcp::acceptor acceptor_;
  std::vector<StubResponse> responses_;
  mutable std::mutex mutex_;
  std::vector<RecordedRequest> requests_;
  std::string failure_;
  std::thread thread_;
};

ToxiproxyClientConfig config_for(const ToxiproxyApiStub& server) {
  ToxiproxyClientConfig config;
  config.api_port = server.port();
  config.proxy_name = "arena-test-redis";
  config.listen = "0.0.0.0:26379";
  config.upstream = "redis:6379";
  config.request_timeout_ms = 1'000;
  return config;
}

TEST(ToxiproxyClientTest, ProbeUsesOnlyTheFixedVersionEndpoint) {
  ToxiproxyApiStub server({{http::status::ok, "{\"version\":\"2.12.0\"}"}});
  ToxiproxyClient client(config_for(server));

  EXPECT_FALSE(client.probe());
  server.wait();
  const auto requests = server.requests();
  ASSERT_EQ(requests.size(), 1U);
  EXPECT_EQ(requests.front().method, http::verb::get);
  EXPECT_EQ(requests.front().target, "/version");
  EXPECT_TRUE(requests.front().body.empty());
}

TEST(ToxiproxyClientTest, PrepareResetsBeforePopulatingTheConfiguredProxy) {
  ToxiproxyApiStub server(
      {{http::status::no_content, {}}, {http::status::created, "{}"}});
  ToxiproxyClient client(config_for(server));

  EXPECT_FALSE(client.prepare());
  server.wait();
  const auto requests = server.requests();
  ASSERT_EQ(requests.size(), 2U);
  EXPECT_EQ(requests[0].method, http::verb::post);
  EXPECT_EQ(requests[0].target, "/reset");
  EXPECT_TRUE(requests[0].body.empty());
  EXPECT_EQ(requests[1].method, http::verb::post);
  EXPECT_EQ(requests[1].target, "/populate");
  EXPECT_EQ(nlohmann::json::parse(requests[1].body),
            nlohmann::json::array({{{"name", "arena-test-redis"},
                                    {"listen", "0.0.0.0:26379"},
                                    {"upstream", "redis:6379"},
                                    {"enabled", true}}}));
}

TEST(ToxiproxyClientTest, InjectUsesOnlyPredefinedProxyMutationsAndToxics) {
  ToxiproxyApiStub server(std::vector<StubResponse>(4U));
  ToxiproxyClient client(config_for(server));

  EXPECT_FALSE(client.inject(ToxiproxyFaultKind::kDisconnect));
  EXPECT_FALSE(client.inject(ToxiproxyFaultKind::kLatency, 125U));
  EXPECT_FALSE(client.inject(ToxiproxyFaultKind::kTimeout, 350U));
  EXPECT_FALSE(client.inject(ToxiproxyFaultKind::kResetPeer, 10U));
  server.wait();
  const auto requests = server.requests();
  ASSERT_EQ(requests.size(), 4U);

  EXPECT_EQ(requests[0].target, "/proxies/arena-test-redis");
  EXPECT_EQ(nlohmann::json::parse(requests[0].body),
            nlohmann::json({{"enabled", false}}));

  const std::vector<std::string> types{"latency", "timeout", "reset_peer"};
  const std::vector<std::uint32_t> values{125U, 350U, 10U};
  for (std::size_t index = 0; index < types.size(); ++index) {
    const auto& recorded = requests[index + 1U];
    EXPECT_EQ(recorded.method, http::verb::post);
    EXPECT_EQ(recorded.target, "/proxies/arena-test-redis/toxics");
    const auto body = nlohmann::json::parse(recorded.body);
    EXPECT_EQ(body.at("name"), "arena-core-lab-" + types[index]);
    EXPECT_EQ(body.at("type"), types[index]);
    EXPECT_EQ(body.at("stream"), "downstream");
    EXPECT_EQ(body.at("toxicity"), 1.0);
    const auto attribute = types[index] == "latency" ? "latency" : "timeout";
    EXPECT_EQ(body.at("attributes").at(attribute), values[index]);
  }
}

TEST(ToxiproxyClientTest, ResetRestoresAllProxiesAndRemovesToxics) {
  ToxiproxyApiStub server({{http::status::no_content, {}}});
  ToxiproxyClient client(config_for(server));

  EXPECT_FALSE(client.reset());
  server.wait();
  const auto requests = server.requests();
  ASSERT_EQ(requests.size(), 1U);
  EXPECT_EQ(requests.front().method, http::verb::post);
  EXPECT_EQ(requests.front().target, "/reset");
}

TEST(ToxiproxyClientTest, RejectsUnsafeConfigurationAndUnboundedFaultValues) {
  ToxiproxyClientConfig config;
  config.proxy_name = "../reset";
  ToxiproxyClient unsafe(std::move(config));
  EXPECT_EQ(unsafe.probe().code, ErrorCode::kConfigValidationFailed);

  ToxiproxyClient bounded(ToxiproxyClientConfig{});
  EXPECT_EQ(bounded.inject(ToxiproxyFaultKind::kLatency, 0U).code,
            ErrorCode::kConfigValidationFailed);
  EXPECT_EQ(bounded.inject(ToxiproxyFaultKind::kTimeout, 60'001U).code,
            ErrorCode::kConfigValidationFailed);
  EXPECT_EQ(bounded.inject(static_cast<ToxiproxyFaultKind>(999), 0U).code,
            ErrorCode::kConfigValidationFailed);
}

TEST(ToxiproxyClientTest, MapsHttpAndNetworkFailuresWithoutReturningResponseSecrets) {
  ToxiproxyApiStub server(
      {{http::status::internal_server_error, "{\"secret\":\"must-not-leak\"}"}});
  ToxiproxyClient rejected(config_for(server));
  const auto http_error = rejected.probe();
  EXPECT_EQ(http_error.code, ErrorCode::kDependencyRedisUnavailable);
  EXPECT_NE(http_error.message.find("500"), std::string::npos);
  EXPECT_EQ(http_error.message.find("must-not-leak"), std::string::npos);
  server.wait();

  asio::io_context io;
  tcp::acceptor unused(io, {asio::ip::address_v4::loopback(), 0});
  const auto unused_port = unused.local_endpoint().port();
  unused.close();
  ToxiproxyClientConfig unavailable_config;
  unavailable_config.api_port = unused_port;
  unavailable_config.request_timeout_ms = 200U;
  const auto network_error = ToxiproxyClient(std::move(unavailable_config)).probe();
  EXPECT_EQ(network_error.code, ErrorCode::kDependencyRedisUnavailable);
  EXPECT_EQ(network_error.message, "Toxiproxy API request failed");
}

TEST(ToxiproxyClientTest, BoundsAStalledHttpResponse) {
  ToxiproxyApiStub server({{http::status::ok, "{}", 600U}});
  auto config = config_for(server);
  config.request_timeout_ms = 100U;
  const auto started = std::chrono::steady_clock::now();
  const auto error = ToxiproxyClient(std::move(config)).probe();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  EXPECT_EQ(error.code, ErrorCode::kDependencyRedisUnavailable);
  EXPECT_LT(elapsed, std::chrono::milliseconds(450));
  server.wait();
}

}  // namespace
}  // namespace arena_core::qa
