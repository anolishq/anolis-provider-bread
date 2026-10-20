#include "devices/common/set_confirm.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#include "crumbs/crumbs_canned_bus.hpp"
#include "crumbs/crumbs_transport.hpp"
#include "crumbs/session.hpp"
#include "devices/common/inventory.hpp"
#include "devices/rlht/rlht_adapter.hpp"

extern "C" {
#include <bread/bread_refuse.h>
#include <bread/rlht_ops.h>

#include "crumbs.h"
}

// SET confirmation from CRUMBS GET_ACK (#134), driven through the RLHT
// adapter's call() over the real transport and codec onto a canned device.

namespace anolis_provider_bread {
namespace {

using Status = anolis::deviceprovider::v1::Status;

constexpr uint8_t kAddr = 0x13;
constexpr uint32_t kSetPeriods = 4u;

class SetConfirmTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto canned = std::make_unique<crumbs::CrumbsCannedBus>("mock://set-confirm-test");
        canned->add_device(kAddr, RLHT_TYPE_ID);
        bus = canned.get();
        transport = std::make_unique<crumbs::CrumbsTransport>(std::move(canned));
        // retry_count 0 keeps every count below exact; the session's own
        // retries are not what is under test.
        session = std::make_unique<crumbs::Session>(*transport,
                                                    crumbs::SessionOptions{"mock://set-confirm-test", 0u, 50u, 0});
        ASSERT_TRUE(session->open());

        device.descriptor.set_device_id("rlht0");
        device.type = DeviceType::Rlht;
        device.address = kAddr;
        device.version.crumbs_version = CRUMBS_ACK_MIN_VERSION;
        device.capability_profile.flags = RLHT_CAP_BASELINE_FLAGS | RLHT_CAP_REFUSES;
    }

    AdapterCallResult set_periods() {
        ValueMap args;
        args["period1_ms"] = make_uint64_val(1000u);
        args["period2_ms"] = make_uint64_val(1000u);
        return rlht::call(*session, device, kSetPeriods, args);
    }

    // One applied SET, so the device holds a record and a lost frame reads as
    // "not advanced" rather than as no record at all.
    void prime() {
        const AdapterCallResult primed = set_periods();
        ASSERT_TRUE(primed.ok) << primed.error_message;
    }

    int sends() const { return bus->writes_of(kAddr, RLHT_OP_SET_PERIODS); }

    crumbs::CrumbsCannedBus *bus = nullptr;
    std::unique_ptr<crumbs::CrumbsTransport> transport;
    std::unique_ptr<crumbs::Session> session;
    inventory::InventoryDevice device;
};

TEST_F(SetConfirmTest, AppliedIsOk) {
    const AdapterCallResult result = set_periods();
    ASSERT_TRUE(result.ok) << result.error_message;
    EXPECT_EQ(sends(), 1);
}

struct RefusalCase {
    uint8_t reason;
    Status::Code code;
    const char *name;
};

class SetConfirmRefusalTest : public SetConfirmTest, public ::testing::WithParamInterface<RefusalCase> {};

TEST_P(SetConfirmRefusalTest, RefusedIsAnErrorNamingTheReasonAndIsNotResent) {
    const RefusalCase c = GetParam();
    prime();
    bus->refuse_next(kAddr, c.reason);

    const AdapterCallResult result = set_periods();
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error_code, c.code);
    EXPECT_NE(result.error_message.find(std::string("refused: ") + c.name), std::string::npos) << result.error_message;
    EXPECT_EQ(sends(), 2) << "a refused SET must not be re-sent";
}

INSTANTIATE_TEST_SUITE_P(
    ByCause, SetConfirmRefusalTest,
    ::testing::Values(RefusalCase{BREAD_REFUSE_ESTOP, Status::CODE_FAILED_PRECONDITION, "ESTOP"},
                      RefusalCase{BREAD_REFUSE_WATCHDOG_TRIP, Status::CODE_FAILED_PRECONDITION, "WATCHDOG_TRIP"},
                      RefusalCase{BREAD_REFUSE_WRONG_MODE, Status::CODE_FAILED_PRECONDITION, "WRONG_MODE"},
                      RefusalCase{BREAD_REFUSE_MALFORMED, Status::CODE_INVALID_ARGUMENT, "MALFORMED"},
                      RefusalCase{BREAD_REFUSE_UNDEFINED_VALUE, Status::CODE_INVALID_ARGUMENT, "UNDEFINED_VALUE"},
                      RefusalCase{BREAD_REFUSE_UNKNOWN_OP, Status::CODE_INVALID_ARGUMENT, "UNKNOWN_OP"},
                      RefusalCase{BREAD_REFUSE_NOT_BUILT, Status::CODE_UNIMPLEMENTED, "NOT_BUILT"},
                      RefusalCase{0x80, Status::CODE_FAILED_PRECONDITION, "0x80"}));

TEST_F(SetConfirmTest, RejectedIsInternalNamingTheReason) {
    prime();
    bus->reject_next(kAddr, CRUMBS_ACK_REJECT_NO_HANDLER);
    const AdapterCallResult no_handler = set_periods();
    EXPECT_FALSE(no_handler.ok);
    EXPECT_EQ(no_handler.error_code, Status::CODE_INTERNAL);
    EXPECT_NE(no_handler.error_message.find("rejected: NO_HANDLER"), std::string::npos) << no_handler.error_message;

    bus->reject_next(kAddr, CRUMBS_ACK_REJECT_TYPE);
    const AdapterCallResult type = set_periods();
    EXPECT_EQ(type.error_code, Status::CODE_INTERNAL);
    EXPECT_NE(type.error_message.find("rejected: TYPE"), std::string::npos) << type.error_message;
    EXPECT_EQ(sends(), 3);
}

TEST_F(SetConfirmTest, NotAdvancedIsResentAndAppliedOnTheSecondSend) {
    prime();
    bus->drop_next(kAddr, 1);
    const AdapterCallResult result = set_periods();
    ASSERT_TRUE(result.ok) << result.error_message;
    EXPECT_EQ(sends(), 1 + 2);
}

TEST_F(SetConfirmTest, NotAdvancedOnEverySendStopsAtTheBound) {
    prime();
    bus->drop_next(kAddr, 10);
    const AdapterCallResult result = set_periods();
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error_code, Status::CODE_UNAVAILABLE);
    EXPECT_NE(result.error_message.find("after 3 sends"), std::string::npos) << result.error_message;
    EXPECT_EQ(sends(), 1 + kConfirmedSends);
}

TEST_F(SetConfirmTest, AckReadFailureAfterTheSendIsNotResent) {
    prime();
    bus->fail_ack_reads(kAddr, 5, /*skip=*/1);  // the baseline reads, the one after the send fails
    const AdapterCallResult result = set_periods();
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error_code, Status::CODE_UNAVAILABLE);
    EXPECT_NE(result.error_message.find("GET_ACK after send failed"), std::string::npos) << result.error_message;
    EXPECT_EQ(sends(), 1 + 1) << "the frame may have landed: it must not be sent again";
}

TEST_F(SetConfirmTest, BaselineFailureSendsNothing) {
    bus->fail_ack_reads(kAddr, 5);
    const AdapterCallResult result = set_periods();
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error_code, Status::CODE_UNAVAILABLE);
    EXPECT_NE(result.error_message.find("not sent"), std::string::npos) << result.error_message;
    EXPECT_EQ(sends(), 0);
}

TEST_F(SetConfirmTest, FirstSetLostBeforeAnyRecordIsNotResent) {
    // No record yet (status NONE): lost and applied-then-restarted read the
    // same, so CRUMBS calls it unknown and nothing is re-sent.
    bus->drop_next(kAddr, 1);
    const AdapterCallResult result = set_periods();
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error_code, Status::CODE_UNAVAILABLE);
    EXPECT_NE(result.error_message.find("cannot say"), std::string::npos) << result.error_message;
    EXPECT_EQ(sends(), 1);
}

TEST_F(SetConfirmTest, DeviceBeforeAckMinVersionIsWrittenOnceUnconfirmed) {
    device.version.crumbs_version = CRUMBS_ACK_MIN_VERSION - 1;
    ASSERT_EQ(set_confirmation(device), SetConfirmation::Unconfirmed);
    bus->drop_next(kAddr, 1);  // lost, and nothing can tell

    const AdapterCallResult result = set_periods();
    ASSERT_TRUE(result.ok) << result.error_message;
    EXPECT_EQ(sends(), 1);
    const crumbs::AddressStats stats = session->stats_for(kAddr);
    EXPECT_EQ(stats.ok + stats.failed, 1U) << "only the write: no GET_ACK query";
}

TEST_F(SetConfirmTest, DeliveryOnlyFirmwareIsOkAndStillResendsWhenNotAdvanced) {
    device.capability_profile.flags = RLHT_CAP_BASELINE_FLAGS;  // no RLHT_CAP_REFUSES
    ASSERT_EQ(set_confirmation(device), SetConfirmation::DeliveryOnly);
    prime();
    bus->drop_next(kAddr, 1);
    const AdapterCallResult result = set_periods();
    ASSERT_TRUE(result.ok) << result.error_message;
    EXPECT_EQ(sends(), 1 + 2);
}

TEST_F(SetConfirmTest, ConfirmationModeFollowsVersionAndRefusesCap) {
    EXPECT_EQ(set_confirmation(device), SetConfirmation::Acknowledged);
    EXPECT_EQ(to_string(SetConfirmation::Acknowledged), "acknowledged");
    EXPECT_EQ(to_string(SetConfirmation::DeliveryOnly), "delivery_only");
    EXPECT_EQ(to_string(SetConfirmation::Unconfirmed), "unconfirmed");

    inventory::InventoryDevice dcmt;
    dcmt.type = DeviceType::Dcmt;
    dcmt.version.crumbs_version = CRUMBS_ACK_MIN_VERSION;
    dcmt.capability_profile.flags = RLHT_CAP_REFUSES;  // RLHT's bit 9 is not DCMT's bit 8
    EXPECT_EQ(set_confirmation(dcmt), SetConfirmation::DeliveryOnly);
}

TEST_F(SetConfirmTest, MockCostOfConfirmation) {
    // Mock path only: the canned bus has no bus time and no query delay, so
    // this is the provider-side cost of the two GET_ACK queries, not a
    // hardware figure.
    constexpr int kCalls = 2000;
    const auto time_calls = [&]() {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kCalls; ++i) {
            if (!set_periods().ok) {
                return -1.0;
            }
        }
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / kCalls;
    };
    const double confirmed_us = time_calls();
    device.version.crumbs_version = CRUMBS_ACK_MIN_VERSION - 1;
    const double unconfirmed_us = time_calls();
    ASSERT_GT(confirmed_us, 0.0);
    ASSERT_GT(unconfirmed_us, 0.0);
    std::cout << "[mock] per SET: confirmed " << confirmed_us << " us, unconfirmed " << unconfirmed_us << " us\n";
}

}  // namespace
}  // namespace anolis_provider_bread
