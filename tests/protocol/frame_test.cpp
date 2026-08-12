#include "arena_core/protocol/frame.hpp"

#include <array>
#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "arena_core.pb.h"

namespace arena_core::protocol {
namespace {

namespace wire = arena_core::protocol::v1;

TEST(FrameTest, EncodesLengthAsFourByteBigEndian) {
  EXPECT_EQ(encode_frame_length(0x01020304U), (std::array<std::uint8_t, 4>{1, 2, 3, 4}));
  const auto length = decode_frame_length({0, 1, 0, 0});
  ASSERT_TRUE(length.ok());
  EXPECT_EQ(*length.value, 65'536U);
}

TEST(FrameTest, RejectsZeroAndOversizedPayloads) {
  EXPECT_EQ(decode_frame_length({0, 0, 0, 0}).error.code, ErrorCode::kProtocolFrameTooLarge);
  EXPECT_EQ(decode_frame_length({0, 1, 0, 1}).error.code, ErrorCode::kProtocolFrameTooLarge);
}

TEST(FrameTest, FramesAndParsesEnvelopeWithoutChangingPayload) {
  wire::Hello hello;
  hello.set_player_id("player-1");
  wire::Envelope envelope;
  envelope.set_protocol_version(1);
  envelope.set_message_type(wire::HELLO);
  envelope.set_request_id(7);
  ASSERT_TRUE(set_payload(envelope, hello));

  const auto framed = frame_message(envelope);
  ASSERT_TRUE(framed.ok());
  std::array<std::uint8_t, 4> header{};
  for (std::size_t index = 0; index < header.size(); ++index) {
    header[index] = static_cast<std::uint8_t>((*framed.value)[index]);
  }
  const auto length = decode_frame_length(header);
  ASSERT_TRUE(length.ok());
  auto parsed = parse_envelope(std::string_view(*framed.value).substr(4));
  ASSERT_TRUE(parsed.ok());
  EXPECT_EQ(parsed.value->request_id(), 7U);
  const auto parsed_hello = parse_payload<wire::Hello>(*parsed.value);
  ASSERT_TRUE(parsed_hello.ok());
  EXPECT_EQ(parsed_hello.value->player_id(), "player-1");
}

TEST(FrameTest, RejectsMalformedEnvelopeAndNestedPayload) {
  EXPECT_EQ(parse_envelope("not-protobuf").error.code, ErrorCode::kProtocolDecodeFailed);
  wire::Envelope envelope;
  envelope.set_payload("not-a-Hello");
  EXPECT_EQ(parse_payload<wire::Hello>(envelope).error.code, ErrorCode::kProtocolDecodeFailed);
}

TEST(FrameTest, FingerprintIncludesTypeTokenAndPayloadButNotRequestId) {
  wire::Envelope first;
  first.set_message_type(wire::HEARTBEAT);
  first.set_request_id(1);
  first.set_session_token("token");
  first.set_payload("payload");
  auto second = first;
  second.set_request_id(999);
  EXPECT_EQ(request_fingerprint(first), request_fingerprint(second));
  second.set_session_token("other");
  EXPECT_NE(request_fingerprint(first), request_fingerprint(second));
}

}  // namespace
}  // namespace arena_core::protocol
