#include "devices/common/safe_outputs.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

#include "crumbs/crumbs_canned_bus.hpp"
#include "crumbs/crumbs_transport.hpp"
#include "crumbs/session.hpp"
#include "devices/common/inventory.hpp"
#include "devices/common/set_confirm.hpp"

extern "C" {
#include <bread/bread_refuse.h>
#include <bread/dcmt_ops.h>
#include <bread/rlht_ops.h>

#include "crumbs.h"
}

// The safe-output sequence (#195), sent over the real transport and codec to
// canned devices, each SET confirmed from GET_ACK as setter calls are.

namespace anolis_provider_bread {
namespace {

constexpr uint8_t kRlhtAddr = 0x08;
constexpr uint8_t kDcmtAddr = 0x09;

class SafeOutputsTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto canned = std::make_unique<crumbs::CrumbsCannedBus>("mock://safe-outputs-test");
        canned->add_device(kRlhtAddr, RLHT_TYPE_ID);
        canned->add_device(kDcmtAddr, DCMT_TYPE_ID);
        bus = canned.get();
        transport = std::make_unique<crumbs::CrumbsTransport>(std::move(canned));
        session = std::make_unique<crumbs::Session>(*transport,
                                                    crumbs::SessionOptions{"mock://safe-outputs-test", 0u, 50u, 0});
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

    crumbs::CrumbsCannedBus *bus = nullptr;
    std::unique_ptr<crumbs::CrumbsTransport> transport;
    std::unique_ptr<crumbs::Session> session;
    inventory::InventoryDevice rlht;
    inventory::InventoryDevice dcmt;
};

TEST_F(SafeOutputsTest, ADcmtIsPutInOpenLoopThenStopped) {
    const safe_outputs::Result result = safe_outputs::drive(*session, dcmt);
    EXPECT_TRUE(result.ok) << result.failures;
    EXPECT_EQ(safe_outputs::to_metric(result), "set");
    EXPECT_EQ(bus->writes_of(kDcmtAddr, DCMT_OP_SET_MODE), 1);
    EXPECT_EQ(bus->writes_of(kDcmtAddr, DCMT_OP_SET_OPEN_LOOP), 1);
    EXPECT_EQ(bus->writes_of(kDcmtAddr, DCMT_OP_SET_BRAKE), 0);  // brakes are left as they are
}

TEST_F(SafeOutputsTest, AnRlhtGetsZeroSetpointsOpenLoopAndZeroDuty) {
    const safe_outputs::Result result = safe_outputs::drive(*session, rlht);
    EXPECT_TRUE(result.ok) << result.failures;
    EXPECT_EQ(bus->writes_of(kRlhtAddr, RLHT_OP_SET_SETPOINTS), 1);
    EXPECT_EQ(bus->writes_of(kRlhtAddr, RLHT_OP_SET_MODE), 1);
    EXPECT_EQ(bus->writes_of(kRlhtAddr, RLHT_OP_SET_OPEN_DUTY), 1);
}

TEST_F(SafeOutputsTest, ARefusedStepIsReportedAndTheRestAreStillSent) {
    bus->refuse_next(kRlhtAddr, BREAD_REFUSE_ESTOP);  // set_setpoints
    const safe_outputs::Result result = safe_outputs::drive(*session, rlht);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.failures.find("set_setpoints: "), std::string::npos) << result.failures;
    EXPECT_NE(result.failures.find("ESTOP"), std::string::npos) << result.failures;
    EXPECT_EQ(safe_outputs::to_metric(result).rfind("not set: set_setpoints: ", 0), 0u);
    EXPECT_EQ(bus->writes_of(kRlhtAddr, RLHT_OP_SET_MODE), 1);
    EXPECT_EQ(bus->writes_of(kRlhtAddr, RLHT_OP_SET_OPEN_DUTY), 1);
}

TEST_F(SafeOutputsTest, AStepThatNeverArrivesIsReported) {
    // A frame lost every send, kConfirmedSends of them, is never applied.
    const safe_outputs::Result primed = safe_outputs::drive(*session, dcmt);  // gives GET_ACK a record
    ASSERT_TRUE(primed.ok) << primed.failures;
    bus->drop_next(kDcmtAddr, kConfirmedSends);  // set_mode
    const safe_outputs::Result result = safe_outputs::drive(*session, dcmt);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.failures.rfind("set_mode: ", 0), 0u) << result.failures;
    EXPECT_EQ(result.failures.find("set_open_loop"), std::string::npos) << result.failures;
}

}  // namespace
}  // namespace anolis_provider_bread
