/**
 * @file crumbs_canned_bus_test.cpp
 * @brief The canned CRUMBS bus + the mock-through-bus decode path (bread#97) and
 * fault injection over it (anolishq/anolis#99).
 *
 * The former MockTransport synthesized finished RawFrames, so mock never ran the
 * exact-length trim + decode #97 is about. These tests prove the canned bus
 * emits real, CRC'd, padded wire bytes and that CrumbsTransport::read runs the
 * real crumbs_frame_length + crumbs_decode_message on them — and that an
 * injected corruption breaks that decode instead of silently passing.
 */

#include "crumbs/crumbs_canned_bus.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "anolis/provider_sdk/i2c/fault_injecting_i2c_bus.hpp"
#include "crumbs/crumbs_transport.hpp"
#include "crumbs/session.hpp"

extern "C" {
#include <bread/dcmt_ops.h>
#include <bread/rlht_ops.h>

#include "crumbs.h"
}

namespace anolis_provider_bread::crumbs {
namespace {

namespace sdk_i2c = anolis::provider_sdk::i2c;

constexpr uint8_t kRlhtAddr = 0x08;

SessionOptions test_options() {
    SessionOptions o;
    o.bus_path = "mock://canned-test";
    o.timeout_ms = 50;
    o.query_delay_us = 0;
    o.retry_count = 0;
    return o;
}

// A SET_REPLY request frame asking the device to stage a given GET reply.
RawFrame set_reply(uint8_t reply_opcode) {
    RawFrame f;
    f.type_id = 0;
    f.opcode = kSetReplyOpcode;
    f.payload = {reply_opcode};
    return f;
}

TEST(CrumbsCannedBusTest, EmitsPaddedFrameThatTheRealDecoderAccepts) {
    // Drive the canned bus directly: seed an RLHT device, record a GET_STATE
    // query, then read the raw bytes it returns.
    CrumbsCannedBus bus("mock://canned-test");
    bus.add_device(kRlhtAddr, RLHT_TYPE_ID);
    ASSERT_TRUE(static_cast<bool>(bus.open()));

    // Encode a SET_REPLY(RLHT_OP_GET_STATE) request and write it (as the transport
    // would), so the bus stages the RLHT reply.
    crumbs_message_t req{};
    req.type_id = 0;
    req.opcode = kSetReplyOpcode;
    req.data_len = 1;
    req.data[0] = RLHT_OP_GET_STATE;
    uint8_t req_buf[CRUMBS_MESSAGE_MAX_SIZE] = {0};
    const size_t req_len = crumbs_encode_message(&req, req_buf, sizeof(req_buf));
    ASSERT_GT(req_len, 0u);
    ASSERT_TRUE(static_cast<bool>(bus.write(kRlhtAddr, req_buf, req_len)));

    // Read the fixed count the transport uses.
    uint8_t rx[CRUMBS_MESSAGE_MAX_SIZE] = {0};
    size_t received = 0;
    ASSERT_TRUE(static_cast<bool>(bus.read(kRlhtAddr, rx, sizeof(rx), &received, 0)));
    EXPECT_EQ(received, static_cast<size_t>(CRUMBS_MESSAGE_MAX_SIZE));  // padded to the requested count

    // The real trim + exact-length decode must accept it — the path MockTransport
    // never exercised (bread#97).
    std::size_t frame_len = 0;
    ASSERT_EQ(crumbs_frame_length(rx, received, &frame_len), 0);
    EXPECT_LT(frame_len, received);  // there IS padding to trim
    crumbs_context_t ctx{};
    crumbs_message_t decoded{};
    crumbs_init(&ctx, CRUMBS_ROLE_CONTROLLER, 0);
    ASSERT_EQ(crumbs_decode_message(rx, frame_len, &decoded, &ctx), 0);
    EXPECT_EQ(decoded.type_id, static_cast<uint8_t>(RLHT_TYPE_ID));
    EXPECT_EQ(decoded.opcode, static_cast<uint8_t>(RLHT_OP_GET_STATE));
    EXPECT_EQ(decoded.data_len, 19u);
    EXPECT_EQ(ctx.last_crc_ok, 1u);
}

TEST(CrumbsCannedBusTest, RoundTripsThroughCrumbsTransportDecode) {
    // The real integration: send + read through CrumbsTransport over the canned
    // bus. The reply is produced as wire bytes and decoded by the real path.
    auto canned = std::make_unique<CrumbsCannedBus>("mock://canned-test");
    canned->add_device(kRlhtAddr, RLHT_TYPE_ID);
    CrumbsTransport transport(std::move(canned));
    ASSERT_TRUE(transport.open(test_options()).ok());

    ASSERT_TRUE(transport.send(kRlhtAddr, set_reply(RLHT_OP_GET_STATE)).ok());
    RawFrame reply;
    const SessionStatus status = transport.read(kRlhtAddr, reply, 50000);
    ASSERT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(reply.type_id, static_cast<uint8_t>(RLHT_TYPE_ID));
    EXPECT_EQ(reply.opcode, static_cast<uint8_t>(RLHT_OP_GET_STATE));
    EXPECT_EQ(reply.payload.size(), 19u);
}

TEST(CrumbsCannedBusFaultTest, CorruptInjectionBreaksTheDecode) {
    // corrupt_every=1 flips a byte of the read frame; the CRC no longer matches,
    // so the real decoder rejects it (DecodeFailed) instead of silently passing.
    auto canned = std::make_unique<CrumbsCannedBus>("mock://canned-test");
    canned->add_device(kRlhtAddr, RLHT_TYPE_ID);
    sdk_i2c::FaultSpec spec;
    spec.corrupt_every = 1;
    auto faulted = std::make_unique<sdk_i2c::FaultInjectingI2cBus>(std::move(canned), spec);
    CrumbsTransport transport(std::move(faulted));
    ASSERT_TRUE(transport.open(test_options()).ok());

    ASSERT_TRUE(transport.send(kRlhtAddr, set_reply(RLHT_OP_GET_STATE)).ok());
    RawFrame reply;
    const SessionStatus status = transport.read(kRlhtAddr, reply, 50000);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code, SessionErrorCode::DecodeFailed);
}

TEST(CrumbsCannedBusFaultTest, InjectedReadFailCountsIoFailed) {
    // read_fail_every=1 fails the read at the transport; CrumbsTransport surfaces
    // ReadFailed and the decorator records it as io_failed.
    auto canned = std::make_unique<CrumbsCannedBus>("mock://canned-test");
    canned->add_device(kRlhtAddr, RLHT_TYPE_ID);
    sdk_i2c::FaultSpec spec;
    spec.read_fail_every = 1;
    auto faulted = std::make_unique<sdk_i2c::FaultInjectingI2cBus>(std::move(canned), spec);
    auto *faulted_raw = faulted.get();
    CrumbsTransport transport(std::move(faulted));
    ASSERT_TRUE(transport.open(test_options()).ok());

    ASSERT_TRUE(transport.send(kRlhtAddr, set_reply(RLHT_OP_GET_STATE)).ok());
    RawFrame reply;
    const SessionStatus status = transport.read(kRlhtAddr, reply, 50000);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code, SessionErrorCode::ReadFailed);
    EXPECT_GE(faulted_raw->io_stats_for(kRlhtAddr).failed, 1u);
}

}  // namespace
}  // namespace anolis_provider_bread::crumbs
