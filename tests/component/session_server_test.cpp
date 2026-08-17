#include "arena_core/protocol/frame.hpp"
#include "arena_core/session/tcp_server.hpp"
#include "arena_core/session/token_generator.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "arena_core.pb.h"

namespace arena_core::session {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
namespace wire = arena_core::protocol::v1;
using namespace std::chrono_literals;

class RunningServer {
 public:
  RunningServer() {
    SessionOptions options;
    options.durations.hello_deadline = 150ms;
    options.durations.idle_timeout = 200ms;
    options.durations.reconnect_window = 180ms;
    options.request_cache_ttl = 5s;
    server_ = std::make_shared<TcpServer>(
        io_context_, tcp::endpoint(asio::ip::address_v4::loopback(), 0), options,
        std::make_shared<DeterministicTokenGenerator>());
    const auto error = server_->start();
    if (error) {
      throw std::runtime_error(error.message);
    }
    port_ = server_->bound_port();
    thread_ = std::thread([this] { io_context_.run(); });
  }

  ~RunningServer() {
    if (thread_.joinable()) {
      if (!io_context_.stopped()) {
        std::promise<void> stopped;
        auto completion = stopped.get_future();
        asio::post(io_context_, [this, &stopped] {
          server_->stop();
          asio::post(io_context_, [&stopped] { stopped.set_value(); });
        });
        completion.wait_for(250ms);
      }
      io_context_.stop();
      thread_.join();
    }
  }

  std::uint16_t port() const noexcept { return port_; }
  std::shared_ptr<TcpServer> server() const { return server_; }

 private:
  asio::io_context io_context_;
  std::shared_ptr<TcpServer> server_;
  std::thread thread_;
  std::uint16_t port_{0};
};

class TestClient {
 public:
  explicit TestClient(const std::uint16_t port) : socket_(io_context_) {
    socket_.connect(tcp::endpoint(asio::ip::address_v4::loopback(), port));
  }

  void send(const wire::Envelope& envelope) {
    auto frame = protocol::frame_message(envelope);
    ASSERT_TRUE(frame.ok()) << frame.error.message;
    asio::write(socket_, asio::buffer(*frame.value));
  }

  std::string frame(const wire::Envelope& envelope) {
    auto framed = protocol::frame_message(envelope);
    EXPECT_TRUE(framed.ok()) << framed.error.message;
    return framed.ok() ? std::move(*framed.value) : std::string{};
  }

  wire::Envelope receive() {
    std::array<std::uint8_t, 4> header{};
    asio::read(socket_, asio::buffer(header));
    auto length = protocol::decode_frame_length(header);
    EXPECT_TRUE(length.ok()) << length.error.message;
    if (!length.ok()) {
      return {};
    }
    std::string payload(*length.value, '\0');
    asio::read(socket_, asio::buffer(payload));
    auto envelope = protocol::parse_envelope(payload);
    EXPECT_TRUE(envelope.ok()) << envelope.error.message;
    return envelope.ok() ? std::move(*envelope.value) : wire::Envelope{};
  }

  void write_bytes(const std::string_view bytes) { asio::write(socket_, asio::buffer(bytes)); }

  boost::system::error_code read_after_close() {
    std::array<char, 1> byte{};
    boost::system::error_code error;
    socket_.read_some(asio::buffer(byte), error);
    return error;
  }

  void close() {
    boost::system::error_code ignored;
    socket_.shutdown(tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
  }

 private:
  asio::io_context io_context_;
  tcp::socket socket_;
};

wire::Envelope hello_request(const std::uint64_t request_id,
                             const std::string& player_id,
                             const std::string& reconnect_token = {}) {
  wire::Hello hello;
  hello.set_player_id(player_id);
  hello.set_reconnect_token(reconnect_token);
  wire::Envelope envelope;
  envelope.set_protocol_version(1);
  envelope.set_message_type(wire::HELLO);
  envelope.set_request_id(request_id);
  protocol::set_payload(envelope, hello);
  return envelope;
}

wire::HelloAck perform_hello(TestClient& client,
                             const std::string& player_id = "player-1",
                             const std::string& reconnect_token = {}) {
  client.send(hello_request(1, player_id, reconnect_token));
  const auto response = client.receive();
  EXPECT_EQ(response.message_type(), wire::HELLO_ACK);
  auto ack = protocol::parse_payload<wire::HelloAck>(response);
  EXPECT_TRUE(ack.ok()) << ack.error.message;
  return ack.ok() ? std::move(*ack.value) : wire::HelloAck{};
}

wire::Envelope heartbeat_request(const std::uint64_t request_id,
                                 const std::string& session_token,
                                 const std::uint64_t client_time = 0) {
  wire::Heartbeat heartbeat;
  heartbeat.set_client_time_ms(client_time);
  wire::Envelope envelope;
  envelope.set_protocol_version(1);
  envelope.set_message_type(wire::HEARTBEAT);
  envelope.set_request_id(request_id);
  envelope.set_session_token(session_token);
  protocol::set_payload(envelope, heartbeat);
  return envelope;
}

wire::ErrorResponse expect_error(const wire::Envelope& envelope, const std::string& code) {
  EXPECT_EQ(envelope.message_type(), wire::ERROR_RESPONSE);
  auto error = protocol::parse_payload<wire::ErrorResponse>(envelope);
  EXPECT_TRUE(error.ok()) << error.error.message;
  if (error.ok()) {
    EXPECT_EQ(error.value->code(), code);
    return std::move(*error.value);
  }
  return {};
}

TEST(SessionServerTest, AcceptsFragmentedHelloAndHeartbeat) {
  RunningServer server;
  TestClient client(server.port());
  const auto hello_frame = client.frame(hello_request(1, "player-1"));
  client.write_bytes(std::string_view(hello_frame).substr(0, 2));
  std::this_thread::sleep_for(5ms);
  client.write_bytes(std::string_view(hello_frame).substr(2));
  const auto hello_response = client.receive();
  ASSERT_EQ(hello_response.message_type(), wire::HELLO_ACK);
  const auto ack = protocol::parse_payload<wire::HelloAck>(hello_response);
  ASSERT_TRUE(ack.ok());

  client.send(heartbeat_request(2, ack.value->session_token(), 123));
  const auto heartbeat_response = client.receive();
  EXPECT_EQ(heartbeat_response.message_type(), wire::HEARTBEAT_ACK);
  EXPECT_EQ(heartbeat_response.request_id(), 2U);
}

TEST(SessionServerTest, SeparatesCoalescedFrames) {
  RunningServer server;
  TestClient client(server.port());
  const auto ack = perform_hello(client);
  const auto first = client.frame(heartbeat_request(2, ack.session_token(), 1));
  const auto second = client.frame(heartbeat_request(3, ack.session_token(), 2));
  client.write_bytes(first + second);
  EXPECT_EQ(client.receive().request_id(), 2U);
  EXPECT_EQ(client.receive().request_id(), 3U);
}

TEST(SessionServerTest, RejectsOversizedAndMalformedFramesThenCloses) {
  RunningServer server;
  {
    TestClient client(server.port());
    const auto header = protocol::encode_frame_length(65'537);
    client.write_bytes(std::string(reinterpret_cast<const char*>(header.data()), header.size()));
    expect_error(client.receive(), "PROTOCOL_FRAME_TOO_LARGE");
    EXPECT_EQ(client.read_after_close(), asio::error::eof);
  }
  {
    TestClient client(server.port());
    const auto header = protocol::encode_frame_length(3);
    std::string malformed(reinterpret_cast<const char*>(header.data()), header.size());
    malformed += "bad";
    client.write_bytes(malformed);
    expect_error(client.receive(), "PROTOCOL_DECODE_FAILED");
    EXPECT_EQ(client.read_after_close(), asio::error::eof);
  }
}

TEST(SessionServerTest, RejectsUnsupportedVersionAndHeartbeatBeforeHello) {
  RunningServer server;
  {
    TestClient client(server.port());
    auto hello = hello_request(1, "player-1");
    hello.set_protocol_version(2);
    client.send(hello);
    expect_error(client.receive(), "PROTOCOL_UNSUPPORTED_VERSION");
    EXPECT_EQ(client.read_after_close(), asio::error::eof);
  }
  {
    TestClient client(server.port());
    client.send(heartbeat_request(1, "fake-token"));
    expect_error(client.receive(), "SESSION_HELLO_REQUIRED");
    EXPECT_EQ(client.read_after_close(), asio::error::eof);
  }
}

TEST(SessionServerTest, ClosesConnectionWhenHelloDeadlineExpires) {
  RunningServer server;
  TestClient client(server.port());
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(client.read_after_close(), asio::error::eof);
  EXPECT_EQ(server.server()->active_connection_count(), 0U);
}

TEST(SessionServerTest, ReplaysSameRequestAndClosesOnRequestIdConflict) {
  RunningServer server;
  TestClient client(server.port());
  const auto ack = perform_hello(client);
  const auto heartbeat = heartbeat_request(2, ack.session_token(), 10);
  client.send(heartbeat);
  const auto first = client.receive();
  client.send(heartbeat);
  const auto replay = client.receive();
  EXPECT_EQ(first.SerializeAsString(), replay.SerializeAsString());

  client.send(heartbeat_request(2, ack.session_token(), 11));
  expect_error(client.receive(), "PROTOCOL_REQUEST_ID_CONFLICT");
  EXPECT_EQ(client.read_after_close(), asio::error::eof);
}

TEST(SessionServerTest, ReconnectsWithinWindowAndRotatesBothTokens) {
  RunningServer server;
  std::string reconnect_token;
  std::string first_session_token;
  {
    TestClient first(server.port());
    const auto ack = perform_hello(first);
    reconnect_token = ack.reconnect_token();
    first_session_token = ack.session_token();
    EXPECT_EQ(ack.generation(), 1U);
    first.close();
  }
  std::this_thread::sleep_for(30ms);

  TestClient resumed(server.port());
  const auto ack = perform_hello(resumed, "player-1", reconnect_token);
  EXPECT_TRUE(ack.resumed());
  EXPECT_EQ(ack.generation(), 2U);
  EXPECT_NE(ack.session_token(), first_session_token);
  EXPECT_NE(ack.reconnect_token(), reconnect_token);
  resumed.send(heartbeat_request(2, ack.session_token()));
  EXPECT_EQ(resumed.receive().message_type(), wire::HEARTBEAT_ACK);
}

TEST(SessionServerTest, RejectsReconnectAtExpiredBoundary) {
  RunningServer server;
  std::string reconnect_token;
  {
    TestClient first(server.port());
    reconnect_token = perform_hello(first).reconnect_token();
    first.close();
  }
  std::this_thread::sleep_for(230ms);
  TestClient late(server.port());
  late.send(hello_request(1, "player-1", reconnect_token));
  expect_error(late.receive(), "SESSION_RECONNECT_EXPIRED");
}

TEST(SessionServerTest, IdleSessionIsClosedAndRegistryEventuallyExpires) {
  RunningServer server;
  TestClient client(server.port());
  perform_hello(client);
  std::this_thread::sleep_for(250ms);
  EXPECT_EQ(client.read_after_close(), asio::error::eof);
  EXPECT_EQ(server.server()->active_connection_count(), 0U);
  std::this_thread::sleep_for(220ms);
  EXPECT_EQ(server.server()->reconnectable_session_count(), 0U);
}

TEST(SessionServerTest, ServerStopIsIdempotentWithAnOpenSession) {
  RunningServer server;
  TestClient client(server.port());
  perform_hello(client);
  server.server()->stop();
  server.server()->stop();
  EXPECT_EQ(client.read_after_close(), asio::error::eof);
}

TEST(SessionRegistryTest, StaleGenerationCannotRevokeResumedSession) {
  auto tokens = std::make_shared<DeterministicTokenGenerator>();
  SessionRegistry registry(tokens);
  const auto now = domain::TimePoint{};
  auto first = registry.open_new("player-1", now);
  ASSERT_TRUE(first.ok());
  registry.mark_disconnected(first.value->reconnect_token, 1, now + 1s);

  auto resumed = registry.resume("player-1", first.value->reconnect_token, now + 100ms);
  ASSERT_TRUE(resumed.ok());
  EXPECT_EQ(resumed.value->generation, 2U);
  registry.revoke(first.value->reconnect_token, 1);
  registry.mark_disconnected(first.value->reconnect_token, 1, now + 1s);
  EXPECT_EQ(registry.size(), 1U);

  registry.revoke(resumed.value->reconnect_token, 2);
  EXPECT_EQ(registry.size(), 0U);
}

TEST(SessionRegistryTest, SweptReconnectTokenRetainsStableExpiredMeaning) {
  auto tokens = std::make_shared<DeterministicTokenGenerator>();
  SessionRegistry registry(tokens);
  const auto now = domain::TimePoint{};
  auto opened = registry.open_new("player-1", now);
  ASSERT_TRUE(opened.ok());
  registry.mark_disconnected(opened.value->reconnect_token, opened.value->generation,
                             now + 100ms);

  EXPECT_EQ(registry.sweep_expired(now + 100ms), 1U);
  EXPECT_EQ(registry.size(), 0U);
  const auto expired = registry.resume("player-1", opened.value->reconnect_token,
                                       now + 200ms);
  ASSERT_FALSE(expired.ok());
  EXPECT_EQ(expired.error.code, ErrorCode::kSessionReconnectExpired);

  auto replacement = registry.open_new("player-1", now + 200ms);
  EXPECT_TRUE(replacement.ok());
  const auto forgotten = registry.resume("player-1", opened.value->reconnect_token,
                                         now + std::chrono::minutes(2));
  ASSERT_FALSE(forgotten.ok());
  EXPECT_EQ(forgotten.error.code, ErrorCode::kSessionTokenInvalid);
}

}  // namespace
}  // namespace arena_core::session
