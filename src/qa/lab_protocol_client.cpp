#include "arena_core/qa/lab_protocol_client.hpp"

#include "arena_core/protocol/frame.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

namespace arena_core::qa {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
namespace wire = arena_core::protocol::v1;
using SteadyClock = std::chrono::steady_clock;

Error client_error(const ErrorCode code, std::string message) {
  return make_error(code, std::move(message));
}

Error server_error(const wire::ErrorResponse& response) {
  ErrorCode code = ErrorCode::kProtocolDecodeFailed;
  if (response.code() == error_code_name(ErrorCode::kSessionReconnectExpired)) {
    code = ErrorCode::kSessionReconnectExpired;
  } else if (response.code() == error_code_name(ErrorCode::kSessionTokenInvalid)) {
    code = ErrorCode::kSessionTokenInvalid;
  } else if (response.code() == error_code_name(ErrorCode::kSessionPlayerIdInvalid)) {
    code = ErrorCode::kSessionPlayerIdInvalid;
  } else if (response.code() == error_code_name(ErrorCode::kSessionNotActive)) {
    code = ErrorCode::kSessionNotActive;
  } else if (response.code() == error_code_name(ErrorCode::kProtocolUnsupportedVersion)) {
    code = ErrorCode::kProtocolUnsupportedVersion;
  }
  auto error = make_error(code, response.message().empty() ? response.code() : response.message());
  error.retryable = response.retryable();
  return error;
}

struct AsyncState {
  bool completed{false};
  boost::system::error_code error;
};

struct ResolveState : AsyncState {
  std::optional<tcp::resolver::results_type> endpoints;
};

struct ReadState : AsyncState {
  std::shared_ptr<std::vector<std::uint8_t>> bytes;
};

}  // namespace

class LabProtocolClient::Impl final {
 public:
  Impl(std::string host,
       const std::uint16_t port,
       std::string player_id,
       std::shared_ptr<LabCancellation> cancellation,
       const SteadyClock::time_point scenario_deadline,
       LabProtocolClientOptions options)
      : host_(std::move(host)),
        port_(port),
        player_id_(std::move(player_id)),
        cancellation_(std::move(cancellation)),
        scenario_deadline_(scenario_deadline),
        options_(options),
        resolver_(io_context_),
        socket_(io_context_) {}

  ~Impl() { (void)disconnect(); }

  Error connect() {
    if (!reconnect_token_.empty()) {
      return client_error(ErrorCode::kSessionNotActive,
                          "client already owns a reconnect token; use reconnect()");
    }
    return establish(false);
  }

  Error reconnect() {
    if (reconnect_token_.empty()) {
      return client_error(ErrorCode::kSessionNotActive,
                          "reconnect requires a token from a previous HelloAck");
    }
    return establish(true);
  }

  Error disconnect() noexcept {
    boost::system::error_code cancel_error;
    resolver_.cancel();
    socket_.cancel(cancel_error);
    boost::system::error_code close_error;
    socket_.close(close_error);
    connected_ = false;
    transport_connected_ = false;
    pending_.clear();
    if (close_error && close_error != asio::error::bad_descriptor) {
      return client_error(ErrorCode::kInternalError,
                          "lab protocol disconnect failed: " + close_error.message());
    }
    return {};
  }

  bool connected() const noexcept { return connected_; }
  bool resumed() const noexcept { return resumed_; }
  bool credentials_rotated() const noexcept { return credentials_rotated_; }
  std::uint64_t generation() const noexcept { return generation_; }

  Result<LabPreparedEnvelope> prepare_request(
      const wire::MessageType type,
      const google::protobuf::MessageLite& payload,
      const std::optional<std::uint64_t> requested_id) {
    if (!connected_) {
      return Result<LabPreparedEnvelope>::failure(
          client_error(ErrorCode::kSessionNotActive, "client is not connected"));
    }
    if (type == wire::HELLO || type == wire::HELLO_ACK || type == wire::MESSAGE_TYPE_UNSPECIFIED) {
      return Result<LabPreparedEnvelope>::failure(client_error(
          ErrorCode::kProtocolUnknownMessage, "Hello envelopes are managed internally"));
    }
    if (requested_id.has_value() && *requested_id == 0U) {
      return Result<LabPreparedEnvelope>::failure(client_error(
          ErrorCode::kProtocolDecodeFailed, "request_id zero is reserved for server pushes"));
    }

    wire::Envelope envelope;
    envelope.set_protocol_version(1U);
    envelope.set_message_type(type);
    const auto request_id = requested_id.value_or(next_request_id_++);
    envelope.set_request_id(request_id);
    envelope.set_session_token(session_token_);
    if (!payload.SerializeToString(envelope.mutable_payload())) {
      return Result<LabPreparedEnvelope>::failure(client_error(
          ErrorCode::kProtocolDecodeFailed, "request payload could not be serialized"));
    }
    if (request_id >= next_request_id_ && request_id != std::numeric_limits<std::uint64_t>::max()) {
      next_request_id_ = request_id + 1U;
    }
    return Result<LabPreparedEnvelope>::success(LabPreparedEnvelope(std::move(envelope)));
  }

  Error send(const LabPreparedEnvelope& request) {
    if (!connected_) {
      return client_error(ErrorCode::kSessionNotActive, "client is not connected");
    }
    auto frame = protocol::frame_message(request.envelope_);
    if (!frame.ok()) return frame.error;
    return write_bytes(std::move(*frame.value), operation_deadline(), "write request");
  }

  Result<wire::Envelope> receive_response(const std::uint64_t request_id) {
    if (request_id == 0U) {
      return Result<wire::Envelope>::failure(client_error(
          ErrorCode::kProtocolDecodeFailed, "request_id zero is not a response id"));
    }
    return receive_matching([request_id](const wire::Envelope& envelope) {
      return envelope.request_id() == request_id;
    });
  }

  Result<wire::Envelope> receive_push(const wire::MessageType type) {
    return receive_matching([type](const wire::Envelope& envelope) {
      return envelope.request_id() == 0U && envelope.message_type() == type;
    });
  }

  Result<wire::RoomStateChanged> receive_room_state(const std::string& expected_state) {
    for (;;) {
      auto envelope = receive_push(wire::ROOM_STATE_CHANGED);
      if (!envelope.ok()) {
        return Result<wire::RoomStateChanged>::failure(std::move(envelope.error));
      }
      auto state = protocol::parse_payload<wire::RoomStateChanged>(*envelope.value);
      if (!state.ok()) return state;
      if (expected_state.empty() || state.value->state() == expected_state) return state;
    }
  }

 private:
  template <typename Predicate>
  Result<wire::Envelope> receive_matching(Predicate predicate) {
    if (!connected_) {
      return Result<wire::Envelope>::failure(
          client_error(ErrorCode::kSessionNotActive, "client is not connected"));
    }

    const auto stored = std::find_if(pending_.begin(), pending_.end(), predicate);
    if (stored != pending_.end()) {
      auto envelope = std::move(*stored);
      pending_.erase(stored);
      return Result<wire::Envelope>::success(std::move(envelope));
    }

    const auto deadline = operation_deadline();
    for (;;) {
      auto envelope = read_envelope(deadline);
      if (!envelope.ok()) return envelope;
      if (predicate(*envelope.value)) return envelope;
      pending_.push_back(std::move(*envelope.value));
    }
  }

  Error establish(const bool resume) {
    if (connected_ || transport_connected_) {
      return client_error(ErrorCode::kSessionNotActive, "client is already connected");
    }
    if (const auto config_error = validate_options()) return config_error;
    if (is_cancelled()) return cancelled_error("connect");
    if (SteadyClock::now() >= scenario_deadline_) return timeout_error("connect");

    auto error = connect_transport(operation_deadline());
    if (error) return error;
    transport_connected_ = true;

    wire::Hello hello;
    hello.set_player_id(player_id_);
    if (resume) hello.set_reconnect_token(reconnect_token_);
    wire::Envelope envelope;
    envelope.set_protocol_version(1U);
    envelope.set_message_type(wire::HELLO);
    envelope.set_request_id(next_request_id_++);
    if (!protocol::set_payload(envelope, hello)) {
      (void)disconnect();
      return client_error(ErrorCode::kProtocolDecodeFailed,
                          "Hello payload could not be serialized");
    }

    auto frame = protocol::frame_message(envelope);
    if (!frame.ok()) {
      (void)disconnect();
      return frame.error;
    }
    error = write_bytes(std::move(*frame.value), operation_deadline(), "write Hello");
    if (error) {
      (void)disconnect();
      return error;
    }

    auto response = read_envelope(operation_deadline());
    if (!response.ok()) {
      (void)disconnect();
      return response.error;
    }
    if (response.value->request_id() == envelope.request_id() &&
        response.value->message_type() == wire::ERROR_RESPONSE) {
      auto parsed_error = protocol::parse_payload<wire::ErrorResponse>(*response.value);
      (void)disconnect();
      if (!parsed_error.ok()) return parsed_error.error;
      return server_error(*parsed_error.value);
    }
    if (response.value->request_id() != envelope.request_id() ||
        response.value->message_type() != wire::HELLO_ACK) {
      const auto actual = wire::MessageType_Name(response.value->message_type());
      (void)disconnect();
      return client_error(ErrorCode::kProtocolDecodeFailed,
                          "Hello expected HELLO_ACK, received " + actual);
    }
    auto ack = protocol::parse_payload<wire::HelloAck>(*response.value);
    if (!ack.ok()) {
      (void)disconnect();
      return ack.error;
    }
    if (ack.value->session_token().empty() || ack.value->reconnect_token().empty() ||
        ack.value->generation() == 0U || ack.value->resumed() != resume) {
      (void)disconnect();
      return client_error(ErrorCode::kProtocolDecodeFailed,
                          "HelloAck token, generation, or resumed state is invalid");
    }

    const auto credentials_rotated = resume &&
        ack.value->session_token() != session_token_ &&
        ack.value->reconnect_token() != reconnect_token_;
    session_token_ = ack.value->session_token();
    reconnect_token_ = ack.value->reconnect_token();
    resumed_ = ack.value->resumed();
    credentials_rotated_ = credentials_rotated;
    generation_ = ack.value->generation();
    pending_.clear();
    connected_ = true;
    return {};
  }

  Error validate_options() const {
    if (host_.empty() || port_ == 0U || player_id_.empty()) {
      return client_error(ErrorCode::kConfigValidationFailed,
                          "lab protocol host, port, and player_id are required");
    }
    if (options_.operation_timeout <= std::chrono::milliseconds::zero() ||
        options_.poll_interval <= std::chrono::milliseconds::zero()) {
      return client_error(ErrorCode::kConfigValidationFailed,
                          "lab protocol timeouts must be positive");
    }
    return {};
  }

  SteadyClock::time_point operation_deadline() const noexcept {
    const auto local = SteadyClock::now() + options_.operation_timeout;
    return std::min(local, scenario_deadline_);
  }

  bool is_cancelled() const noexcept {
    return cancellation_ && cancellation_->requested();
  }

  Error cancelled_error(const std::string& operation) const {
    return client_error(ErrorCode::kQaRunCancelled,
                        "lab protocol " + operation + " cancelled");
  }

  Error timeout_error(const std::string& operation) const {
    return client_error(ErrorCode::kQaRunTimedOut,
                        "lab protocol " + operation + " timed out");
  }

  Error io_error(const std::string& operation,
                 const boost::system::error_code& error) const {
    return client_error(ErrorCode::kInternalError,
                        "lab protocol " + operation + " failed: " + error.message());
  }

  template <typename State, typename Abort>
  Error wait_for(const std::shared_ptr<State>& state,
                 const SteadyClock::time_point deadline,
                 const std::string& operation,
                 Abort abort) {
    for (;;) {
      if (state->completed) {
        if (state->error) return io_error(operation, state->error);
        return {};
      }
      if (is_cancelled()) {
        abort();
        return cancelled_error(operation);
      }
      const auto now = SteadyClock::now();
      if (now >= deadline) {
        abort();
        return timeout_error(operation);
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      const auto slice = std::max(std::chrono::milliseconds(1),
                                  std::min(options_.poll_interval, remaining));
      io_context_.restart();
      (void)io_context_.run_one_for(slice);
    }
  }

  Error connect_transport(const SteadyClock::time_point deadline) {
    auto resolve = std::make_shared<ResolveState>();
    resolver_.async_resolve(host_, std::to_string(port_),
        [resolve](const boost::system::error_code& error,
                  tcp::resolver::results_type endpoints) {
          resolve->error = error;
          if (!error) resolve->endpoints.emplace(std::move(endpoints));
          resolve->completed = true;
        });
    auto error = wait_for(resolve, deadline, "resolve", [this] { resolver_.cancel(); });
    if (error) return error;
    if (!resolve->endpoints.has_value()) {
      return client_error(ErrorCode::kInternalError,
                          "lab protocol resolve returned no endpoints");
    }

    boost::system::error_code close_error;
    socket_.close(close_error);
    auto connect = std::make_shared<AsyncState>();
    asio::async_connect(socket_, *resolve->endpoints,
        [connect](const boost::system::error_code& connect_error,
                  const tcp::endpoint&) {
          connect->error = connect_error;
          connect->completed = true;
        });
    return wait_for(connect, deadline, "connect", [this] {
      boost::system::error_code ignored;
      socket_.cancel(ignored);
      socket_.close(ignored);
    });
  }

  Error write_bytes(std::string bytes,
                    const SteadyClock::time_point deadline,
                    const std::string& operation) {
    if (!socket_.is_open()) {
      return client_error(ErrorCode::kSessionNotActive,
                          "lab protocol transport is not connected");
    }
    auto owned_bytes = std::make_shared<std::string>(std::move(bytes));
    auto state = std::make_shared<AsyncState>();
    asio::async_write(socket_, asio::buffer(*owned_bytes),
        [state, owned_bytes](const boost::system::error_code& error, std::size_t) {
          state->error = error;
          state->completed = true;
        });
    const auto result = wait_for(state, deadline, operation, [this] {
      boost::system::error_code ignored;
      socket_.cancel(ignored);
      socket_.close(ignored);
      connected_ = false;
      transport_connected_ = false;
    });
    return result;
  }

  Result<std::vector<std::uint8_t>> read_bytes(
      const std::size_t size,
      const SteadyClock::time_point deadline,
      const std::string& operation) {
    auto state = std::make_shared<ReadState>();
    state->bytes = std::make_shared<std::vector<std::uint8_t>>(size);
    asio::async_read(socket_, asio::buffer(*state->bytes),
        [state](const boost::system::error_code& error, std::size_t) {
          state->error = error;
          state->completed = true;
        });
    const auto error = wait_for(state, deadline, operation, [this] {
      boost::system::error_code ignored;
      socket_.cancel(ignored);
      socket_.close(ignored);
      connected_ = false;
      transport_connected_ = false;
    });
    if (error) return Result<std::vector<std::uint8_t>>::failure(error);
    return Result<std::vector<std::uint8_t>>::success(std::move(*state->bytes));
  }

  Result<wire::Envelope> read_envelope(const SteadyClock::time_point deadline) {
    auto header = read_bytes(protocol::kFrameHeaderBytes, deadline, "read frame header");
    if (!header.ok()) return Result<wire::Envelope>::failure(std::move(header.error));
    std::array<std::uint8_t, protocol::kFrameHeaderBytes> fixed_header{};
    std::copy(header.value->begin(), header.value->end(), fixed_header.begin());
    auto length = protocol::decode_frame_length(fixed_header);
    if (!length.ok()) return Result<wire::Envelope>::failure(std::move(length.error));
    auto payload = read_bytes(*length.value, deadline, "read frame payload");
    if (!payload.ok()) return Result<wire::Envelope>::failure(std::move(payload.error));
    const std::string serialized(payload.value->begin(), payload.value->end());
    return protocol::parse_envelope(serialized);
  }

  std::string host_;
  std::uint16_t port_{0};
  std::string player_id_;
  std::shared_ptr<LabCancellation> cancellation_;
  SteadyClock::time_point scenario_deadline_;
  LabProtocolClientOptions options_;
  asio::io_context io_context_;
  tcp::resolver resolver_;
  tcp::socket socket_;
  std::string session_token_;
  std::string reconnect_token_;
  std::uint64_t next_request_id_{1};
  std::uint64_t generation_{0};
  bool resumed_{false};
  bool credentials_rotated_{false};
  bool transport_connected_{false};
  bool connected_{false};
  std::vector<wire::Envelope> pending_;
};

LabProtocolClient::LabProtocolClient(
    std::string host,
    const std::uint16_t port,
    std::string player_id,
    std::shared_ptr<LabCancellation> cancellation,
    const std::chrono::steady_clock::time_point scenario_deadline,
    LabProtocolClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(host), port, std::move(player_id),
                                  std::move(cancellation), scenario_deadline, options)) {}

LabProtocolClient::~LabProtocolClient() = default;
LabProtocolClient::LabProtocolClient(LabProtocolClient&&) noexcept = default;
LabProtocolClient& LabProtocolClient::operator=(LabProtocolClient&&) noexcept = default;

Error LabProtocolClient::connect() {
  try {
    return impl_->connect();
  } catch (const std::exception& exception) {
    return client_error(ErrorCode::kInternalError,
                        "lab protocol connect raised an exception: " + std::string(exception.what()));
  } catch (...) {
    return client_error(ErrorCode::kInternalError,
                        "lab protocol connect raised an unknown exception");
  }
}

Error LabProtocolClient::disconnect() noexcept { return impl_->disconnect(); }

Error LabProtocolClient::reconnect() {
  try {
    return impl_->reconnect();
  } catch (const std::exception& exception) {
    return client_error(ErrorCode::kInternalError,
                        "lab protocol reconnect raised an exception: " + std::string(exception.what()));
  } catch (...) {
    return client_error(ErrorCode::kInternalError,
                        "lab protocol reconnect raised an unknown exception");
  }
}

bool LabProtocolClient::connected() const noexcept { return impl_->connected(); }
bool LabProtocolClient::resumed() const noexcept { return impl_->resumed(); }
bool LabProtocolClient::credentials_rotated() const noexcept {
  return impl_->credentials_rotated();
}
std::uint64_t LabProtocolClient::generation() const noexcept { return impl_->generation(); }

Result<LabPreparedEnvelope> LabProtocolClient::prepare_request(
    const protocol::v1::MessageType type,
    const google::protobuf::MessageLite& payload,
    const std::optional<std::uint64_t> request_id) {
  try {
    return impl_->prepare_request(type, payload, request_id);
  } catch (const std::exception& exception) {
    return Result<LabPreparedEnvelope>::failure(client_error(
        ErrorCode::kInternalError,
        "lab protocol prepare raised an exception: " + std::string(exception.what())));
  } catch (...) {
    return Result<LabPreparedEnvelope>::failure(client_error(
        ErrorCode::kInternalError, "lab protocol prepare raised an unknown exception"));
  }
}

Error LabProtocolClient::send(const LabPreparedEnvelope& request) {
  try {
    return impl_->send(request);
  } catch (const std::exception& exception) {
    return client_error(ErrorCode::kInternalError,
                        "lab protocol write raised an exception: " + std::string(exception.what()));
  } catch (...) {
    return client_error(ErrorCode::kInternalError,
                        "lab protocol write raised an unknown exception");
  }
}

Result<protocol::v1::Envelope> LabProtocolClient::receive_response(
    const std::uint64_t request_id) {
  try {
    return impl_->receive_response(request_id);
  } catch (const std::exception& exception) {
    return Result<protocol::v1::Envelope>::failure(client_error(
        ErrorCode::kInternalError,
        "lab protocol read raised an exception: " + std::string(exception.what())));
  } catch (...) {
    return Result<protocol::v1::Envelope>::failure(client_error(
        ErrorCode::kInternalError, "lab protocol read raised an unknown exception"));
  }
}

Result<protocol::v1::Envelope> LabProtocolClient::receive_push(
    const protocol::v1::MessageType type) {
  try {
    return impl_->receive_push(type);
  } catch (const std::exception& exception) {
    return Result<protocol::v1::Envelope>::failure(client_error(
        ErrorCode::kInternalError,
        "lab protocol push read raised an exception: " + std::string(exception.what())));
  } catch (...) {
    return Result<protocol::v1::Envelope>::failure(client_error(
        ErrorCode::kInternalError, "lab protocol push read raised an unknown exception"));
  }
}

Result<protocol::v1::RoomStateChanged> LabProtocolClient::receive_room_state(
    const std::string& expected_state) {
  try {
    return impl_->receive_room_state(expected_state);
  } catch (const std::exception& exception) {
    return Result<protocol::v1::RoomStateChanged>::failure(client_error(
        ErrorCode::kInternalError,
        "lab protocol room-state read raised an exception: " + std::string(exception.what())));
  } catch (...) {
    return Result<protocol::v1::RoomStateChanged>::failure(client_error(
        ErrorCode::kInternalError, "lab protocol room-state read raised an unknown exception"));
  }
}

}  // namespace arena_core::qa
