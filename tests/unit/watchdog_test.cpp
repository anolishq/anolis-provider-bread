#include "devices/common/watchdog.hpp"

#include <gtest/gtest.h>

#include <deque>
#include <memory>
#include <vector>

#include "crumbs/crumbs_canned_bus.hpp"
#include "crumbs/crumbs_transport.hpp"
#include "crumbs/session.hpp"
#include "devices/common/device_adapter.hpp"
#include "devices/common/inventory.hpp"

extern "C" {
#include <bread/bread_watchdog.h>
#include <bread/dcmt_ops.h>
#include <bread/rlht_ops.h>
}

namespace anolis_provider_bread::watchdog {
namespace {

class FakeTransport final : public crumbs::Transport {
public:
    struct ReadAction {
        crumbs::SessionStatus status = crumbs::SessionStatus::success();
        crumbs::RawFrame frame;
    };

    crumbs::SessionStatus open(const crumbs::SessionOptions &) override {
        open_state = true;
        return crumbs::SessionStatus::success();
    }
    void close() noexcept override { open_state = false; }
    bool is_open() const override { return open_state; }
    crumbs::SessionStatus scan(const crumbs::ScanOptions &, std::vector<crumbs::ScanResult> &out) override {
        out.clear();
        return crumbs::SessionStatus::success();
    }
    crumbs::SessionStatus send(uint8_t address, const crumbs::RawFrame &frame) override {
        sent_addresses.push_back(address);
        sent_frames.push_back(frame);
        return crumbs::SessionStatus::success();
    }
    crumbs::SessionStatus read(uint8_t, crumbs::RawFrame &frame, uint32_t) override {
        if (read_actions.empty()) {
            return crumbs::SessionStatus::failure(crumbs::SessionErrorCode::ReadFailed, "no read action");
        }
        ReadAction action = read_actions.front();
        read_actions.pop_front();
        if (action.status) {
            frame = action.frame;
        }
        return action.status;
    }
    void delay_us(uint32_t) override {}

    bool open_state = false;
    std::vector<uint8_t> sent_addresses;
    std::vector<crumbs::RawFrame> sent_frames;
    std::deque<ReadAction> read_actions;
};

inventory::InventoryDevice make_dcmt_device(uint32_t caps_flags, int command_watchdog_ms) {
    inventory::InventoryDevice device;
    device.descriptor.set_device_id("dcmt0");
    device.type = DeviceType::Dcmt;
    device.address = 0x14;
    device.capability_profile.flags = caps_flags;
    device.command_watchdog_ms = command_watchdog_ms;
    return device;
}

TEST(WatchdogTest, CapabilityGateFollowsPerTypeFlag) {
    EXPECT_TRUE(capability_supported(make_dcmt_device(DCMT_CAP_BASELINE_FLAGS | DCMT_CAP_CMD_WATCHDOG, 0)));
    EXPECT_FALSE(capability_supported(make_dcmt_device(DCMT_CAP_BASELINE_FLAGS, 0)));

    inventory::InventoryDevice rlht;
    rlht.type = DeviceType::Rlht;
    rlht.capability_profile.flags = RLHT_CAP_BASELINE_FLAGS | RLHT_CAP_CMD_WATCHDOG;
    EXPECT_TRUE(capability_supported(rlht));
    rlht.capability_profile.flags = RLHT_CAP_BASELINE_FLAGS;
    EXPECT_FALSE(capability_supported(rlht));
}

TEST(WatchdogTest, ArmSendsSetWatchdogFrameLittleEndian) {
    FakeTransport transport;
    crumbs::Session session(transport, crumbs::SessionOptions{"/dev/i2c-1", 10000u, 100u, 0});
    ASSERT_TRUE(session.open());

    arm_if_configured(session, make_dcmt_device(DCMT_CAP_CMD_WATCHDOG, 5000), "test");

    ASSERT_EQ(transport.sent_frames.size(), 1U);
    EXPECT_EQ(transport.sent_addresses[0], 0x14u);
    EXPECT_EQ(transport.sent_frames[0].type_id, DCMT_TYPE_ID);
    EXPECT_EQ(transport.sent_frames[0].opcode, BREAD_OP_SET_WATCHDOG);
    ASSERT_EQ(transport.sent_frames[0].payload.size(), 2U);
    EXPECT_EQ(transport.sent_frames[0].payload[0], 0x88u);  // 5000 = 0x1388 LE
    EXPECT_EQ(transport.sent_frames[0].payload[1], 0x13u);
}

TEST(WatchdogTest, ArmNeverSendsClearTripEvenWhenSupported) {
    // Startup and recovery both arm through arm_if_configured. On latching
    // firmware a trip is cleared only by the operator's clear_watchdog_trip,
    // so arming must send SET_WATCHDOG and nothing else.
    FakeTransport transport;
    crumbs::Session session(transport, crumbs::SessionOptions{"/dev/i2c-1", 10000u, 100u, 0});
    ASSERT_TRUE(session.open());

    arm_if_configured(session, make_dcmt_device(DCMT_CAP_CMD_WATCHDOG | DCMT_CAP_CLEAR_WATCHDOG_TRIP, 5000), "startup");
    arm_if_configured(session, make_dcmt_device(DCMT_CAP_CMD_WATCHDOG | DCMT_CAP_CLEAR_WATCHDOG_TRIP, 5000),
                      "recovery");

    ASSERT_EQ(transport.sent_frames.size(), 2U);
    for (const crumbs::RawFrame &frame : transport.sent_frames) {
        EXPECT_EQ(frame.opcode, BREAD_OP_SET_WATCHDOG);
        EXPECT_NE(frame.opcode, BREAD_OP_CLEAR_WATCHDOG_TRIP);
    }
}

TEST(WatchdogTest, CannedBusTripSurvivesRearmAndClearsOnOperatorCall) {
    // Mock round trip over the real transport + CRUMBS codec: a tripped device
    // stays tripped through a re-arm and reports tripped=0 only after the
    // clear_watchdog_trip function runs.
    constexpr uint8_t kAddr = 0x14;
    auto canned = std::make_unique<crumbs::CrumbsCannedBus>("mock://watchdog-test");
    canned->add_device(kAddr, DCMT_TYPE_ID);
    crumbs::CrumbsCannedBus *bus = canned.get();
    crumbs::CrumbsTransport transport(std::move(canned));
    crumbs::Session session(transport, crumbs::SessionOptions{"mock://watchdog-test", 0u, 50u, 0});
    ASSERT_TRUE(session.open());

    const inventory::InventoryDevice device =
        make_dcmt_device(DCMT_CAP_CMD_WATCHDOG | DCMT_CAP_CLEAR_WATCHDOG_TRIP, 5000);
    arm_if_configured(session, device, "startup");
    bus->trip_watchdog(kAddr);

    WatchdogStatus status;
    ASSERT_TRUE(query_status(session, device, status));
    EXPECT_TRUE(status.armed);
    EXPECT_TRUE(status.tripped);
    EXPECT_EQ(status.trip_count, 1u);

    arm_if_configured(session, device, "recovery");
    ASSERT_TRUE(query_status(session, device, status));
    EXPECT_TRUE(status.tripped) << "re-arming must not release a latched trip";

    const AdapterCallResult cleared = call(adapter_for(device.type), &session, device, 6u, ValueMap{});
    ASSERT_TRUE(cleared.ok) << cleared.error_message;

    ASSERT_TRUE(query_status(session, device, status));
    EXPECT_FALSE(status.tripped);
    EXPECT_TRUE(status.armed);
    EXPECT_EQ(status.timeout_ms, 5000u);
    EXPECT_EQ(status.trip_count, 1u);
}

TEST(WatchdogTest, CannedBusRejectsClearTripWithPayload) {
    // Firmware rejects a CLEAR_WATCHDOG_TRIP carrying a payload and leaves the
    // trip set; the canned bus must do the same.
    constexpr uint8_t kAddr = 0x14;
    auto canned = std::make_unique<crumbs::CrumbsCannedBus>("mock://watchdog-test");
    canned->add_device(kAddr, DCMT_TYPE_ID);
    crumbs::CrumbsCannedBus *bus = canned.get();
    crumbs::CrumbsTransport transport(std::move(canned));
    crumbs::Session session(transport, crumbs::SessionOptions{"mock://watchdog-test", 0u, 50u, 0});
    ASSERT_TRUE(session.open());

    const inventory::InventoryDevice device =
        make_dcmt_device(DCMT_CAP_CMD_WATCHDOG | DCMT_CAP_CLEAR_WATCHDOG_TRIP, 5000);
    arm_if_configured(session, device, "startup");
    bus->trip_watchdog(kAddr);

    crumbs::RawFrame bad_clear;
    bad_clear.type_id = DCMT_TYPE_ID;
    bad_clear.opcode = BREAD_OP_CLEAR_WATCHDOG_TRIP;
    bad_clear.payload = {0x00};
    ASSERT_TRUE(session.send(kAddr, bad_clear));

    WatchdogStatus status;
    ASSERT_TRUE(query_status(session, device, status));
    EXPECT_TRUE(status.tripped) << "a clear with a payload must leave the trip set";
}

TEST(WatchdogTest, ArmIsSkippedWhenUnconfiguredOrUnsupported) {
    FakeTransport transport;
    crumbs::Session session(transport, crumbs::SessionOptions{"/dev/i2c-1", 10000u, 100u, 0});
    ASSERT_TRUE(session.open());

    arm_if_configured(session, make_dcmt_device(DCMT_CAP_CMD_WATCHDOG, 0), "test");
    arm_if_configured(session, make_dcmt_device(DCMT_CAP_BASELINE_FLAGS, 5000), "test");

    EXPECT_TRUE(transport.sent_frames.empty());
}

TEST(WatchdogTest, QueryStatusParsesReply) {
    FakeTransport transport;
    crumbs::Session session(transport, crumbs::SessionOptions{"/dev/i2c-1", 10000u, 100u, 0});
    ASSERT_TRUE(session.open());

    FakeTransport::ReadAction action;
    action.frame.type_id = DCMT_TYPE_ID;
    action.frame.opcode = BREAD_OP_GET_WATCHDOG;
    action.frame.payload = {1, 0x88, 0x13, 1, 3};
    transport.read_actions.push_back(action);

    WatchdogStatus status;
    ASSERT_TRUE(query_status(session, make_dcmt_device(DCMT_CAP_CMD_WATCHDOG, 5000), status));
    EXPECT_TRUE(status.armed);
    EXPECT_EQ(status.timeout_ms, 5000u);
    EXPECT_TRUE(status.tripped);
    EXPECT_EQ(status.trip_count, 3u);
}

TEST(WatchdogTest, QueryStatusRejectsWrongOpcode) {
    FakeTransport transport;
    crumbs::Session session(transport, crumbs::SessionOptions{"/dev/i2c-1", 10000u, 100u, 0});
    ASSERT_TRUE(session.open());

    FakeTransport::ReadAction action;
    action.frame.type_id = DCMT_TYPE_ID;
    action.frame.opcode = DCMT_OP_GET_STATE;
    action.frame.payload = {0};
    transport.read_actions.push_back(action);

    WatchdogStatus status;
    EXPECT_FALSE(query_status(session, make_dcmt_device(DCMT_CAP_CMD_WATCHDOG, 5000), status));
}

}  // namespace
}  // namespace anolis_provider_bread::watchdog
