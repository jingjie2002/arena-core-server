#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/core/result.hpp"
#include "arena_core/qa/lab_run.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <google/protobuf/message_lite.h>

#include "arena_core.pb.h"

namespace arena_core::qa {

struct LabProtocolClientOptions {
  std::chrono::milliseconds operation_timeout{std::chrono::seconds(5)};
  std::chrono::milliseconds poll_interval{std::chrono::milliseconds(5)};
};

// An opaque, copyable request envelope. Keeping the protobuf envelope private
// lets a scenario resend the exact same request without exposing session tokens
// to report/JSON code.
class LabPreparedEnvelope final {
 public:
  LabPreparedEnvelope(const LabPreparedEnvelope&) = default;
  LabPreparedEnvelope& operator=(const LabPreparedEnvelope&) = default;
  LabPreparedEnvelope(LabPreparedEnvelope&&) noexcept = default;
  LabPreparedEnvelope& operator=(LabPreparedEnvelope&&) noexcept = default;

  std::uint64_t request_id() const noexcept { return envelope_.request_id(); }
  protocol::v1::MessageType message_type() const noexcept {
    return envelope_.message_type();
  }

 private:
  explicit LabPreparedEnvelope(protocol::v1::Envelope envelope)
      : envelope_(std::move(envelope)) {}

  protocol::v1::Envelope envelope_;

  friend class LabProtocolClient;
};

// Synchronous facade over deadline-bounded Boost.Asio operations. One instance
// is owned by one scenario worker and is intentionally not thread-safe.
class LabProtocolClient final {
 public:
  LabProtocolClient(std::string host,
                    std::uint16_t port,
                    std::string player_id,
                    std::shared_ptr<LabCancellation> cancellation,
                    std::chrono::steady_clock::time_point scenario_deadline,
                    LabProtocolClientOptions options = {});
  ~LabProtocolClient();

  LabProtocolClient(const LabProtocolClient&) = delete;
  LabProtocolClient& operator=(const LabProtocolClient&) = delete;
  LabProtocolClient(LabProtocolClient&&) noexcept;
  LabProtocolClient& operator=(LabProtocolClient&&) noexcept;

  Error connect();
  Error disconnect() noexcept;
  Error reconnect();

  bool connected() const noexcept;
  bool resumed() const noexcept;
  bool credentials_rotated() const noexcept;
  std::uint64_t generation() const noexcept;

  Result<LabPreparedEnvelope> prepare_request(
      protocol::v1::MessageType type,
      const google::protobuf::MessageLite& payload,
      std::optional<std::uint64_t> request_id = std::nullopt);
  Error send(const LabPreparedEnvelope& request);

  Result<protocol::v1::Envelope> receive_response(std::uint64_t request_id);
  Result<protocol::v1::Envelope> receive_push(protocol::v1::MessageType type);
  Result<protocol::v1::RoomStateChanged> receive_room_state(
      const std::string& expected_state = {});

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace arena_core::qa
