#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <google/protobuf/message_lite.h>

#include "arena_core.pb.h"

namespace arena_core::protocol {

inline constexpr std::size_t kFrameHeaderBytes = 4;
inline constexpr std::size_t kMaximumPayloadBytes = 65'536;

std::array<std::uint8_t, kFrameHeaderBytes> encode_frame_length(std::uint32_t payload_length) noexcept;
Result<std::uint32_t> decode_frame_length(
    const std::array<std::uint8_t, kFrameHeaderBytes>& header,
    std::size_t maximum_payload = kMaximumPayloadBytes);
Result<std::string> frame_message(const google::protobuf::MessageLite& message,
                                  std::size_t maximum_payload = kMaximumPayloadBytes);
Result<v1::Envelope> parse_envelope(std::string_view payload);
std::uint64_t request_fingerprint(const v1::Envelope& envelope) noexcept;

template <typename Message>
Result<Message> parse_payload(const v1::Envelope& envelope) {
  Message message;
  if (!message.ParseFromArray(envelope.payload().data(), static_cast<int>(envelope.payload().size()))) {
    return Result<Message>::failure(make_error(ErrorCode::kProtocolDecodeFailed, "message payload is invalid"));
  }
  return Result<Message>::success(std::move(message));
}

template <typename Message>
bool set_payload(v1::Envelope& envelope, const Message& message) {
  return message.SerializeToString(envelope.mutable_payload());
}

}  // namespace arena_core::protocol
