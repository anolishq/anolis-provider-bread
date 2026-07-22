/**
 * @file crumbs_transport_test.cpp
 * @brief Regression tests for CRUMBS reply framing over Linux I2C.
 *
 * A Linux I2C controller must request a fixed byte count up front, but a CRUMBS
 * peripheral writes only its actual frame and the bus then floats high. So a raw
 * read returns the buffer size with 0xFF padding in the tail — not the frame
 * size, which is what crumbs_decode_message's exact-length contract requires.
 *
 * bread 0.2.9-0.3.0 broke on exactly this (CRUMBS v0.12.4 strictness + raw
 * lengths), and 0.3.1 carried a local trim workaround. Since CRUMBS v0.12.5 the
 * trim lives upstream (crumbs_frame_length, used by CrumbsTransport::read); these
 * tests pin the contract from the consumer side with the literal bytes captured
 * from a Slice_RLHT at 0x0A on the bench Pi (anolishq/anolis#138), so an
 * upstream regression cannot silently reintroduce the breakage. CI is mock://
 * only — exact frames, no bus padding — which is how the original slipped
 * through.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#if defined(__linux__)

#include "crumbs/crumbs_transport.hpp"

namespace anolis_provider_bread::crumbs {
namespace {

// Verbatim from `i2ctransfer -y 1 r31@0x0a` on the bench Pi: a 9-byte version
// reply (type=0x01 opcode=0x00 data_len=5, CRC 0xE3) followed by 22 bytes of
// 0xFF bus padding.
const std::vector<uint8_t> kBenchReply = {0x01, 0x00, 0x05, 0xb4, 0x04, 0x01, 0x00, 0x00, 0xe3, 0xff, 0xff,
                                          0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                          0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

TEST(CrumbsTransportFraming, UpstreamTrimYieldsTheDeclaredFrame) {
    std::size_t frame_len = 0;

    ASSERT_EQ(crumbs_frame_length(kBenchReply.data(), kBenchReply.size(), &frame_len), 0);
    // header(3) + data_len(5) + crc(1) — NOT the 31 bytes the read returned.
    EXPECT_EQ(frame_len, 9u);
    EXPECT_LT(frame_len, kBenchReply.size());
}

TEST(CrumbsTransportFraming, TrimmedLengthDecodesWhileTheRawLengthDoesNot) {
    std::size_t frame_len = 0;
    ASSERT_EQ(crumbs_frame_length(kBenchReply.data(), kBenchReply.size(), &frame_len), 0);

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

TEST(CrumbsTransportFraming, RejectsAnAllPaddingRead) {
    // A silent device: address ACKed, no data supplied, master reads all 0xFF.
    std::vector<uint8_t> garbage(31, 0xff);
    std::size_t frame_len = 0;

    EXPECT_NE(crumbs_frame_length(garbage.data(), garbage.size(), &frame_len), 0);
}

TEST(CrumbsTransportFraming, AcceptsAnExactlySizedFrameWithNoPadding) {
    // An Arduino controller hands over exactly the frame; trimming must be a no-op.
    const std::vector<uint8_t> exact(kBenchReply.begin(), kBenchReply.begin() + 9);
    std::size_t frame_len = 0;

    ASSERT_EQ(crumbs_frame_length(exact.data(), exact.size(), &frame_len), 0);
    EXPECT_EQ(frame_len, exact.size());
}

}  // namespace
}  // namespace anolis_provider_bread::crumbs

#endif  // defined(__linux__)
