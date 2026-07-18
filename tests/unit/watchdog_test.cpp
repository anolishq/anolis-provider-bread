#include "devices/common/watchdog.hpp"

#include <gtest/gtest.h>

#include <deque>
#include <vector>

#include "crumbs/session.hpp"
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
