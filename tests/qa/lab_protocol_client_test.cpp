#include "arena_core/qa/lab_protocol_client.hpp"

#include "arena_core/protocol/frame.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

namespace arena_core::qa {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
namespace wire = arena_core::protocol::v1;
using namespace std::chrono_literals;

wire::Envelope read_envelope(tcp::socket& socket) {
  std::array<std::uint8_t, protocol::kFrameHeaderBytes> header{};
  asio::read(socket, asio::buffer(header));
  auto length = protocol::decode_frame_length(header);
  if (!length.ok()) throw std::runtime_error(length.error.message);
  std::string payload(*length.value, '\0');
  asio::read(socket, asio::buffer(payload));
  auto envelope = protocol::parse_envelope(payload);
  if (!envelope.ok()) throw std::runtime_error(envelope.error.message);
  return std::move(*envelope.value);
}

void write_envelope(tcp::socket& socket, const wire::Envelope& envelope) {
  auto frame = protocol::frame_message(envelope);
  if (!frame.ok()) throw std::runtime_error(frame.error.message);
  asio::write(socket, asio::buffer(*frame.value));
}

wire::Envelope hello_ack(const std::uint64_t request_id,
                         const bool resumed,
                         const std::uint64_t generation,
                         std::string reconnect_token = "reconnect-secret") {
  wire::HelloAck ack;
  ack.set_session_token(resumed ? "session-resumed" : "session-initial");
  ack.set_reconnect_token(std::move(reconnect_token));
  ack.set_resumed(resumed);
  ack.set_generation(generation);
  wire::Envelope envelope;
  envelope.set_protocol_version(1U);
  envelope.set_message_type(wire::HELLO_ACK);
  envelope.set_request_id(request_id);
  if (!protocol::set_payload(envelope, ack)) throw std::runtime_error("HelloAck serialize failed");
  return envelope;
}

wire::Envelope heartbeat_ack(const std::uint64_t request_id) {
  wire::HeartbeatAck ack;
  ack.set_server_time_ms(123U);
  wire::Envelope envelope;
  envelope.set_protocol_version(1U);
  envelope.set_message_type(wire::HEARTBEAT_ACK);
  envelope.set_request_id(request_id);
  if (!protocol::set_payload(envelope, ack)) {
    throw std::runtime_error("HeartbeatAck serialize failed");
  }
  return envelope;
}

wire::Envelope error_response(const std::uint64_t request_id,
                              std::string code,
                              std::string message) {
  wire::ErrorResponse error;
  error.set_code(std::move(code));
  error.set_message(std::move(message));
  error.set_retryable(false);
  wire::Envelope envelope;
  envelope.set_protocol_version(1U);
  envelope.set_message_type(wire::ERROR_RESPONSE);
  envelope.set_request_id(request_id);
  if (!protocol::set_payload(envelope, error)) {
    throw std::runtime_error("ErrorResponse serialize failed");
  }
  return envelope;
}

class FakeTcpServer final {
 public:
  FakeTcpServer()
      : acceptor_(io_context_, tcp::endpoint(tcp::v4(), 0U)), socket_(io_context_) {}

  ~FakeTcpServer() {
    boost::system::error_code ignored;
    socket_.close(ignored);
    acceptor_.close(ignored);
    if (worker_.joinable()) worker_.join();
  }

  std::uint16_t port() const { return acceptor_.local_endpoint().port(); }

  template <typename Handler>
  void start(Handler handler) {
    worker_ = std::thread([this, handler = std::move(handler)]() mutable {
      try {
        handler(*this);
      } catch (...) {
        exception_ = std::current_exception();
      }
    });
  }

  tcp::socket& accept() {
    boost::system::error_code ignored;
    socket_.close(ignored);
    acceptor_.accept(socket_);
    return socket_;
  }

  void finish() {
    if (worker_.joinable()) worker_.join();
    if (exception_) std::rethrow_exception(exception_);
  }

 private:
  asio::io_context io_context_;
  tcp::acceptor acceptor_;
  tcp::socket socket_;
  std::thread worker_;
  std::exception_ptr exception_;
};

TEST(LabProtocolClientTest, PerformsInitialHelloAndResendsIdenticalEnvelope) {
  FakeTcpServer server;
  wire::Hello observed_hello;
  std::string first_serialized;
  std::string second_serialized;
  server.start([&](FakeTcpServer& fake) {
    auto& socket = fake.accept();
    const auto hello_envelope = read_envelope(socket);
    auto hello = protocol::parse_payload<wire::Hello>(hello_envelope);
    if (!hello.ok()) throw std::runtime_error(hello.error.message);
    observed_hello = std::move(*hello.value);
    write_envelope(socket, hello_ack(hello_envelope.request_id(), false, 3U));

    const auto first = read_envelope(socket);
    first_serialized = first.SerializeAsString();
    write_envelope(socket, heartbeat_ack(first.request_id()));
    const auto second = read_envelope(socket);
    second_serialized = second.SerializeAsString();
    write_envelope(socket, heartbeat_ack(second.request_id()));
  });

  auto cancellation = std::make_shared<LabCancellation>();
  LabProtocolClient client("127.0.0.1", server.port(), "player-alpha", cancellation,
                           std::chrono::steady_clock::now() + 2s);
  ASSERT_FALSE(client.connect()) << "initial Hello failed";
  EXPECT_FALSE(client.resumed());
  EXPECT_FALSE(client.credentials_rotated());
  EXPECT_EQ(client.generation(), 3U);

  wire::Heartbeat heartbeat;
  heartbeat.set_client_time_ms(99U);
  auto request = client.prepare_request(wire::HEARTBEAT, heartbeat, 77U);
  ASSERT_TRUE(request.ok()) << request.error.message;
  EXPECT_EQ(request.value->request_id(), 77U);
  ASSERT_FALSE(client.send(*request.value));
  auto first_response = client.receive_response(77U);
  ASSERT_TRUE(first_response.ok()) << first_response.error.message;
  ASSERT_FALSE(client.send(*request.value));
  auto second_response = client.receive_response(77U);
  ASSERT_TRUE(second_response.ok()) << second_response.error.message;
  server.finish();

  EXPECT_EQ(observed_hello.player_id(), "player-alpha");
  EXPECT_TRUE(observed_hello.reconnect_token().empty());
  EXPECT_EQ(first_serialized, second_serialized);
}

TEST(LabProtocolClientTest, ReconnectSendsSavedTokenAndUpdatesResumeMetadata) {
  FakeTcpServer server;
  wire::Hello reconnect_hello;
  server.start([&](FakeTcpServer& fake) {
    auto& first_socket = fake.accept();
    const auto first_hello = read_envelope(first_socket);
    write_envelope(first_socket,
                   hello_ack(first_hello.request_id(), false, 4U, "token-from-first-ack"));

    auto& second_socket = fake.accept();
    const auto second_hello = read_envelope(second_socket);
    auto parsed = protocol::parse_payload<wire::Hello>(second_hello);
    if (!parsed.ok()) throw std::runtime_error(parsed.error.message);
    reconnect_hello = std::move(*parsed.value);
    write_envelope(second_socket,
                   hello_ack(second_hello.request_id(), true, 5U, "rotated-token"));
  });

  auto cancellation = std::make_shared<LabCancellation>();
  LabProtocolClient client("127.0.0.1", server.port(), "player-resume", cancellation,
                           std::chrono::steady_clock::now() + 2s);
  ASSERT_FALSE(client.connect());
  EXPECT_EQ(client.generation(), 4U);
  ASSERT_FALSE(client.disconnect());
  ASSERT_FALSE(client.reconnect());
  EXPECT_TRUE(client.resumed());
  EXPECT_TRUE(client.credentials_rotated());
  EXPECT_EQ(client.generation(), 5U);
  server.finish();

  EXPECT_EQ(reconnect_hello.player_id(), "player-resume");
  EXPECT_EQ(reconnect_hello.reconnect_token(), "token-from-first-ack");
}

TEST(LabProtocolClientTest, MapsReconnectExpiryErrorFromHelloResponse) {
  FakeTcpServer server;
  server.start([](FakeTcpServer& fake) {
    auto& first_socket = fake.accept();
    const auto first_hello = read_envelope(first_socket);
    write_envelope(first_socket,
                   hello_ack(first_hello.request_id(), false, 8U, "expiring-token"));

    auto& second_socket = fake.accept();
    const auto second_hello = read_envelope(second_socket);
    write_envelope(second_socket,
                   error_response(second_hello.request_id(), "SESSION_RECONNECT_EXPIRED",
                                  "reconnect window expired"));
  });

  auto cancellation = std::make_shared<LabCancellation>();
  LabProtocolClient client("127.0.0.1", server.port(), "player-expired", cancellation,
                           std::chrono::steady_clock::now() + 2s);
  ASSERT_FALSE(client.connect());
  ASSERT_FALSE(client.disconnect());
  const auto error = client.reconnect();
  server.finish();

  EXPECT_EQ(error.code, ErrorCode::kSessionReconnectExpired);
  EXPECT_EQ(error.message, "reconnect window expired");
  EXPECT_FALSE(error.retryable);
}

TEST(LabProtocolClientTest, ReadHonorsOperationDeadline) {
  FakeTcpServer server;
  server.start([](FakeTcpServer& fake) {
    auto& socket = fake.accept();
    (void)read_envelope(socket);
    std::this_thread::sleep_for(150ms);
  });

  LabProtocolClientOptions options;
  options.operation_timeout = 40ms;
  options.poll_interval = 2ms;
  auto cancellation = std::make_shared<LabCancellation>();
  LabProtocolClient client("127.0.0.1", server.port(), "player-timeout", cancellation,
                           std::chrono::steady_clock::now() + 2s, options);
  const auto started = std::chrono::steady_clock::now();
  const auto error = client.connect();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  server.finish();

  EXPECT_EQ(error.code, ErrorCode::kQaRunTimedOut) << error.message;
  EXPECT_LT(elapsed, 500ms);
}

TEST(LabProtocolClientTest, ReadHonorsLabCancellation) {
  FakeTcpServer server;
  std::promise<void> hello_received;
  auto hello_received_future = hello_received.get_future();
  server.start([&hello_received](FakeTcpServer& fake) {
    auto& socket = fake.accept();
    (void)read_envelope(socket);
    hello_received.set_value();
    std::this_thread::sleep_for(150ms);
  });

  LabProtocolClientOptions options;
  options.operation_timeout = 1s;
  options.poll_interval = 2ms;
  auto cancellation = std::make_shared<LabCancellation>();
  std::thread canceller([cancellation, &hello_received_future] {
    if (hello_received_future.wait_for(1s) == std::future_status::ready) {
      cancellation->request();
    }
  });
  LabProtocolClient client("127.0.0.1", server.port(), "player-cancel", cancellation,
                           std::chrono::steady_clock::now() + 2s, options);
  const auto error = client.connect();
  canceller.join();
  server.finish();

  EXPECT_EQ(error.code, ErrorCode::kQaRunCancelled) << error.message;
}

}  // namespace
}  // namespace arena_core::qa
