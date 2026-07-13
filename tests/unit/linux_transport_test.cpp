/**
 * @file linux_transport_test.cpp
 * @brief Regression tests for CRUMBS reply framing over Linux I2C.
 *
 * A Linux I2C controller must request a fixed byte count up front, but a CRUMBS
 * peripheral writes only its actual frame and the bus then floats high. So a raw
 * read returns the buffer size with 0xFF padding in the tail — not the frame
 * size, which is what crumbs_decode_message's exact-length contract requires.
 *
 * CRUMBS v0.12.2 did not enforce that contract, so passing the raw length worked.
 * v0.12.4 does enforce it (trailing bytes -> rc=-1), and bread 0.2.9 bumped the
 * pin v0.12.2 -> v0.12.4 — breaking every live read from that release onward.
 *
 * CI missed it because every automated surface uses mock://, which returns exact
 * frames with no bus padding. Only real Linux i2c-dev reproduces it, so these
 * tests use the literal bytes captured from a Slice_RLHT at 0x0A on the bench Pi
 * (anolishq/anolis#138) and assert the raw length is still rejected.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#if defined(__linux__)

#include "crumbs/linux_transport.hpp"

namespace anolis_provider_bread::crumbs {
namespace {

// Verbatim from `i2ctransfer -y 1 r31@0x0a` on the bench Pi: a 9-byte version
// reply (type=0x01 opcode=0x00 data_len=5, CRC 0xE3) followed by 22 bytes of
// 0xFF bus padding.
const std::vector<uint8_t> kBenchReply = {0x01, 0x00, 0x05, 0xb4, 0x04, 0x01, 0x00, 0x00, 0xe3, 0xff, 0xff,
                                          0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                          0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

TEST(LinuxTransportFraming, TrimsPaddedReadToTheDeclaredFrame) {
    std::size_t frame_len = 0;
    const SessionStatus status = crumbs_reply_frame_length(kBenchReply.data(), kBenchReply.size(), frame_len);

    ASSERT_TRUE(status.ok()) << status.message;
    // header(3) + data_len(5) + crc(1) — NOT the 31 bytes the read returned.
    EXPECT_EQ(frame_len, 9u);
    EXPECT_LT(frame_len, kBenchReply.size());
}

TEST(LinuxTransportFraming, TrimmedLengthDecodesWhileTheRawLengthDoesNot) {
    std::size_t frame_len = 0;
    ASSERT_TRUE(crumbs_reply_frame_length(kBenchReply.data(), kBenchReply.size(), frame_len).ok());

    crumbs_context_t ctx{};
    crumbs_message_t message{};
    crumbs_init(&ctx, CRUMBS_ROLE_CONTROLLER, 0);

    // The raw read length is what bread used to pass, and the decoder rejects it.
    EXPECT_NE(crumbs_decode_message(kBenchReply.data(), kBenchReply.size(), &message, &ctx), 0)
        << "the raw read length must still be rejected — that was the bug";

    // The trimmed length decodes, CRC and all.
    crumbs_context_t ctx_trimmed{};
    crumbs_message_t decoded{};
    crumbs_init(&ctx_trimmed, CRUMBS_ROLE_CONTROLLER, 0);
    ASSERT_EQ(crumbs_decode_message(kBenchReply.data(), frame_len, &decoded, &ctx_trimmed), 0);

    EXPECT_EQ(decoded.type_id, 0x01u);
    EXPECT_EQ(decoded.opcode, 0x00u);
    EXPECT_EQ(decoded.data_len, 5u);
    EXPECT_EQ(decoded.crc8, 0xE3u);
    EXPECT_EQ(ctx_trimmed.last_crc_ok, 1u);
}

TEST(LinuxTransportFraming, RejectsAnOutOfRangeDeclaredPayload) {
    // data_len byte is garbage (e.g. an all-0xFF read from a silent device).
    std::vector<uint8_t> garbage(31, 0xff);
    std::size_t frame_len = 0;

    const SessionStatus status = crumbs_reply_frame_length(garbage.data(), garbage.size(), frame_len);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code, SessionErrorCode::DecodeFailed);
}

TEST(LinuxTransportFraming, RejectsATruncatedFrame) {
    // Header declares 5 payload bytes (frame = 9) but only 6 bytes were read.
    const std::vector<uint8_t> truncated = {0x01, 0x00, 0x05, 0xb4, 0x04, 0x01};
    std::size_t frame_len = 0;

    const SessionStatus status = crumbs_reply_frame_length(truncated.data(), truncated.size(), frame_len);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code, SessionErrorCode::ReadFailed);
}

TEST(LinuxTransportFraming, RejectsAReadShorterThanTheMinimumFrame) {
    const std::vector<uint8_t> runt = {0x01, 0x00, 0x05};
    std::size_t frame_len = 0;

    const SessionStatus status = crumbs_reply_frame_length(runt.data(), runt.size(), frame_len);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code, SessionErrorCode::ReadFailed);
}

TEST(LinuxTransportFraming, AcceptsAnExactlySizedFrameWithNoPadding) {
    // An Arduino controller hands over exactly the frame; trimming must be a no-op.
    const std::vector<uint8_t> exact(kBenchReply.begin(), kBenchReply.begin() + 9);
    std::size_t frame_len = 0;

    ASSERT_TRUE(crumbs_reply_frame_length(exact.data(), exact.size(), frame_len).ok());
    EXPECT_EQ(frame_len, exact.size());
}

TEST(LinuxTransportFraming, AcceptsAZeroPayloadFrame) {
    // data_len = 0 -> frame is header + crc only.
    const std::vector<uint8_t> empty_payload = {0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff};
    std::size_t frame_len = 0;

    ASSERT_TRUE(crumbs_reply_frame_length(empty_payload.data(), empty_payload.size(), frame_len).ok());
    EXPECT_EQ(frame_len, 4u);
}

}  // namespace
}  // namespace anolis_provider_bread::crumbs

#endif  // defined(__linux__)
