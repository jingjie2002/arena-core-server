#include "arena_core/protocol/frame.hpp"

#include <limits>

namespace arena_core::protocol {

std::array<std::uint8_t, kFrameHeaderBytes> encode_frame_length(const std::uint32_t payload_length) noexcept {
  return {static_cast<std::uint8_t>((payload_length >> 24U) & 0xFFU),
          static_cast<std::uint8_t>((payload_length >> 16U) & 0xFFU),
          static_cast<std::uint8_t>((payload_length >> 8U) & 0xFFU),
          static_cast<std::uint8_t>(payload_length & 0xFFU)};
}

Result<std::uint32_t> decode_frame_length(const std::array<std::uint8_t, kFrameHeaderBytes>& header,
                                          const std::size_t maximum_payload) {
  const auto length = (static_cast<std::uint32_t>(header[0]) << 24U) |
                      (static_cast<std::uint32_t>(header[1]) << 16U) |
                      (static_cast<std::uint32_t>(header[2]) << 8U) |
                      static_cast<std::uint32_t>(header[3]);
  if (length == 0U || length > maximum_payload) {
    return Result<std::uint32_t>::failure(make_error(ErrorCode::kProtocolFrameTooLarge));
  }
  return Result<std::uint32_t>::success(length);
}

Result<std::string> frame_message(const google::protobuf::MessageLite& message, const std::size_t maximum_payload) {
  const auto serialized_size = message.ByteSizeLong();
  if (serialized_size == 0U || serialized_size > maximum_payload ||
      serialized_size > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    return Result<std::string>::failure(make_error(ErrorCode::kProtocolFrameTooLarge));
  }

  std::string framed(kFrameHeaderBytes + serialized_size, '\0');
  const auto header = encode_frame_length(static_cast<std::uint32_t>(serialized_size));
  for (std::size_t index = 0; index < header.size(); ++index) {
    framed[index] = static_cast<char>(header[index]);
  }
  if (!message.SerializeToArray(framed.data() + kFrameHeaderBytes, static_cast<int>(serialized_size))) {
    return Result<std::string>::failure(make_error(ErrorCode::kProtocolDecodeFailed));
  }
  return Result<std::string>::success(std::move(framed));
}

Result<v1::Envelope> parse_envelope(const std::string_view payload) {
  if (payload.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return Result<v1::Envelope>::failure(make_error(ErrorCode::kProtocolFrameTooLarge));
  }
  v1::Envelope envelope;
  if (!envelope.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
    return Result<v1::Envelope>::failure(make_error(ErrorCode::kProtocolDecodeFailed));
  }
  return Result<v1::Envelope>::success(std::move(envelope));
}

std::uint64_t request_fingerprint(const v1::Envelope& envelope) noexcept {
  constexpr std::uint64_t kOffset = 14'695'981'039'346'656'037ULL;
  constexpr std::uint64_t kPrime = 1'099'511'628'211ULL;
  std::uint64_t hash = kOffset;
  const auto mix = [&hash](const std::string_view bytes) {
    for (const unsigned char value : bytes) {
      hash ^= value;
      hash *= kPrime;
    }
  };

  const auto type = static_cast<std::uint32_t>(envelope.message_type());
  for (unsigned shift = 0; shift < 32; shift += 8) {
    hash ^= static_cast<std::uint8_t>((type >> shift) & 0xFFU);
    hash *= kPrime;
  }
  mix(envelope.session_token());
  mix(envelope.payload());
  return hash;
}

}  // namespace arena_core::protocol
