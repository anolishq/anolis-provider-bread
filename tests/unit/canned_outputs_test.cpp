// The canned device's outputs (anolishq/anolis-provider-bread#195): an applied
// output SET changes what GET_STATE reports, so a mock session can show
// outputs turned off; a refused one leaves them as they were.

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "crumbs/crumbs_canned_bus.hpp"
#include "crumbs/crumbs_transport.hpp"
#include "crumbs/session.hpp"
#include "devices/common/adapter_helpers.hpp"
#include "devices/common/inventory.hpp"
#include "devices/dcmt/dcmt_adapter.hpp"
#include "devices/rlht/rlht_adapter.hpp"

extern "C" {
#include <bread/bread_refuse.h>
#include <bread/dcmt_ops.h>
#include <bread/rlht_ops.h>

#include "crumbs.h"
}

namespace anolis_provider_bread {
namespace {

constexpr uint8_t kRlhtAddr = 0x08;
constexpr uint8_t kDcmtAddr = 0x09;

class CannedOutputsTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto canned = std::make_unique<crumbs::CrumbsCannedBus>("mock://canned-outputs-test");
        canned->add_device(kRlhtAddr, RLHT_TYPE_ID);
        canned->add_device(kDcmtAddr, DCMT_TYPE_ID);
        bus = canned.get();
        transport = std::make_unique<crumbs::CrumbsTransport>(std::move(canned));
        session = std::make_unique<crumbs::Session>(*transport,
                                                    crumbs::SessionOptions{"mock://canned-outputs-test", 0u, 50u, 0});
        ASSERT_TRUE(session->open());

        rlht.descriptor.set_device_id("rlht0");
        rlht.type = DeviceType::Rlht;
        rlht.address = kRlhtAddr;
        rlht.version.crumbs_version = CRUMBS_ACK_MIN_VERSION;
        rlht.capability_profile.flags = RLHT_CAP_BASELINE_FLAGS | RLHT_CAP_REFUSES;

        dcmt.descriptor.set_device_id("dcmt0");
        dcmt.type = DeviceType::Dcmt;
        dcmt.address = kDcmtAddr;
        dcmt.version.crumbs_version = CRUMBS_ACK_MIN_VERSION;
        dcmt.capability_profile.flags = DCMT_CAP_BASELINE_FLAGS | DCMT_CAP_REFUSES;
    }

    const anolis::deviceprovider::v1::Value &signal(const AdapterReadResult &read, const std::string &id) {
        for (const auto &value : read.values) {
            if (value.signal_id() == id) {
                return value.value();
            }
        }
        ADD_FAILURE() << "no signal " << id;
        static const anolis::deviceprovider::v1::Value missing;
        return missing;
    }

    crumbs::CrumbsCannedBus *bus = nullptr;
    std::unique_ptr<crumbs::CrumbsTransport> transport;
    std::unique_ptr<crumbs::Session> session;
    inventory::InventoryDevice rlht;
    inventory::InventoryDevice dcmt;
};

TEST_F(CannedOutputsTest, ADcmtStartsDrivingAndReportsTheOpenLoopPwmItIsSent) {
    AdapterReadResult before = dcmt::read_signals(*session, dcmt, {"motor1_value", "motor2_value"});
    ASSERT_TRUE(before.ok) << before.error_message;
    EXPECT_EQ(signal(before, "motor1_value").int64_value(), 100);
    EXPECT_EQ(signal(before, "motor2_value").int64_value(), -100);

    ValueMap args;
    args["motor1_pwm"] = make_int64_val(0);
    args["motor2_pwm"] = make_int64_val(0);
    const AdapterCallResult sent = dcmt::call(*session, dcmt, 1u, args);  // set_open_loop
    ASSERT_TRUE(sent.ok) << sent.error_message;

    AdapterReadResult after = dcmt::read_signals(*session, dcmt, {"motor1_value", "motor2_value"});
    ASSERT_TRUE(after.ok) << after.error_message;
    EXPECT_EQ(signal(after, "motor1_value").int64_value(), 0);
    EXPECT_EQ(signal(after, "motor2_value").int64_value(), 0);
}

TEST_F(CannedOutputsTest, AnRlhtRelayFollowsItsOpenLoopDuty) {
    AdapterReadResult before = rlht::read_signals(*session, rlht, {"relay1_on", "relay2_on"});
    ASSERT_TRUE(before.ok) << before.error_message;
    EXPECT_TRUE(signal(before, "relay1_on").bool_value());
    EXPECT_FALSE(signal(before, "relay2_on").bool_value());

    ValueMap args;
    args["duty1_pct"] = make_uint64_val(0);
    args["duty2_pct"] = make_uint64_val(40);
    const AdapterCallResult sent = rlht::call(*session, rlht, 6u, args);  // set_open_duty_pct
    ASSERT_TRUE(sent.ok) << sent.error_message;

    AdapterReadResult after = rlht::read_signals(*session, rlht, {"relay1_on", "relay2_on"});
    ASSERT_TRUE(after.ok) << after.error_message;
    EXPECT_FALSE(signal(after, "relay1_on").bool_value());
    EXPECT_TRUE(signal(after, "relay2_on").bool_value());
}

TEST_F(CannedOutputsTest, AnRlhtReportsTheModeAndSetpointsItIsSent) {
    ValueMap mode;
    mode["mode"] = make_string_val("closed_loop");
    ASSERT_TRUE(rlht::call(*session, rlht, 1u, mode).ok);
    ValueMap setpoints;
    setpoints["setpoint1_c"] = make_double_val(0.0);
    setpoints["setpoint2_c"] = make_double_val(12.5);
    ASSERT_TRUE(rlht::call(*session, rlht, 2u, setpoints).ok);

    AdapterReadResult after = rlht::read_signals(*session, rlht, {"mode", "setpoint1_c", "setpoint2_c"});
    ASSERT_TRUE(after.ok) << after.error_message;
    EXPECT_EQ(signal(after, "mode").string_value(), "closed_loop");
    EXPECT_DOUBLE_EQ(signal(after, "setpoint1_c").double_value(), 0.0);
    EXPECT_DOUBLE_EQ(signal(after, "setpoint2_c").double_value(), 12.5);
}

TEST_F(CannedOutputsTest, ARefusedSetLeavesTheOutputsAsTheyWere) {
    bus->refuse_next(kDcmtAddr, BREAD_REFUSE_ESTOP);
    ValueMap args;
    args["motor1_pwm"] = make_int64_val(0);
    args["motor2_pwm"] = make_int64_val(0);
    EXPECT_FALSE(dcmt::call(*session, dcmt, 1u, args).ok);

    AdapterReadResult after = dcmt::read_signals(*session, dcmt, {"motor1_value"});
    ASSERT_TRUE(after.ok) << after.error_message;
    EXPECT_EQ(signal(after, "motor1_value").int64_value(), 100);
}

}  // namespace
}  // namespace anolis_provider_bread
