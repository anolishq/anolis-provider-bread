#include "core/health.hpp"

#include <chrono>
#include <string>

#include <gtest/gtest.h>

#include "core/runtime_state.hpp"
#include "devices/common/inventory.hpp"

namespace anolis_provider_bread::health {
namespace {

using DeviceHState = anolis::deviceprovider::v1::DeviceHealth;
using ProviderHState = anolis::deviceprovider::v1::ProviderHealth;

static runtime::RuntimeState make_base_state() {
    runtime::RuntimeState s;
    s.ready          = true;
    s.startup_message = "test startup ok";
    s.inventory_mode  = "discovered";
    s.started_at      = std::chrono::system_clock::now();
    return s;
}

static inventory::InventoryDevice make_device(const std::string &id) {
    inventory::InventoryDevice d;
    d.descriptor.set_device_id(id);
    return d;
}

// ---------------------------------------------------------------------------
// make_provider_health
// ---------------------------------------------------------------------------

TEST(HealthTest, ProviderHealthIsOkWhenReadyAndNoMissingDevices) {
    const auto state  = make_base_state();
    const auto health = make_provider_health(state);
    EXPECT_EQ(health.state(), ProviderHState::STATE_OK);
    EXPECT_EQ(health.message(), "test startup ok");
}

TEST(HealthTest, ProviderHealthIsDegradedWhenNotReady) {
    auto state  = make_base_state();
    state.ready = false;
    const auto health = make_provider_health(state);
    EXPECT_EQ(health.state(), ProviderHState::STATE_DEGRADED);
}

TEST(HealthTest, ProviderHealthIsDegradedWhenExpectedDevicesMissing) {
    auto state                   = make_base_state();
    state.missing_expected_ids   = {"rlht-0x10", "rlht-0x11"};
    const auto health            = make_provider_health(state);
    EXPECT_EQ(health.state(), ProviderHState::STATE_DEGRADED);
    // Message should mention how many are missing
    EXPECT_NE(health.message().find("2"), std::string::npos);
}

TEST(HealthTest, ProviderHealthMetricsIncludesDegradedKey) {
    auto state                 = make_base_state();
    state.missing_expected_ids = {"rlht-0x10"};
    const auto health          = make_provider_health(state);
    const auto &m              = health.metrics();
    ASSERT_TRUE(m.count("degraded") > 0);
    EXPECT_EQ(m.at("degraded"), "true");
}

TEST(HealthTest, ProviderHealthMetricsDegradedFalseWhenHealthy) {
    const auto state  = make_base_state();
    const auto health = make_provider_health(state);
    const auto &m     = health.metrics();
    ASSERT_TRUE(m.count("degraded") > 0);
    EXPECT_EQ(m.at("degraded"), "false");
}

// ---------------------------------------------------------------------------
// make_device_health
// ---------------------------------------------------------------------------

TEST(HealthTest, DeviceHealthEmptyWhenNoDevicesAndNoMissing) {
    const auto state   = make_base_state();
    const auto healths = make_device_health(state);
    EXPECT_TRUE(healths.empty());
}

TEST(HealthTest, DeviceHealthIncludesPresentDeviceAsOk) {
    auto state     = make_base_state();
    state.devices  = {make_device("rlht-0x08")};
    const auto healths = make_device_health(state);

    ASSERT_EQ(healths.size(), 1u);
    EXPECT_EQ(healths[0].device_id(), "rlht-0x08");
    EXPECT_EQ(healths[0].state(), DeviceHState::STATE_OK);
}

TEST(HealthTest, DeviceHealthIncludesMissingExpectedDeviceAsUnreachable) {
    auto state                 = make_base_state();
    state.missing_expected_ids = {"rlht-0x10"};
    const auto healths         = make_device_health(state);

    ASSERT_EQ(healths.size(), 1u);
    EXPECT_EQ(healths[0].device_id(), "rlht-0x10");
    EXPECT_EQ(healths[0].state(), DeviceHState::STATE_UNREACHABLE);
}

TEST(HealthTest, DeviceHealthCombinesPresentAndMissingDevices) {
    auto state                 = make_base_state();
    state.devices              = {make_device("rlht-0x08")};
    state.missing_expected_ids = {"rlht-0x10"};
    const auto healths         = make_device_health(state);

    ASSERT_EQ(healths.size(), 2u);
    EXPECT_EQ(healths[0].device_id(), "rlht-0x08");
    EXPECT_EQ(healths[0].state(), DeviceHState::STATE_OK);
    EXPECT_EQ(healths[1].device_id(), "rlht-0x10");
    EXPECT_EQ(healths[1].state(), DeviceHState::STATE_UNREACHABLE);
}

TEST(HealthTest, DeviceHealthMissingEntryHasMissingMetric) {
    auto state                 = make_base_state();
    state.missing_expected_ids = {"rlht-0x10"};
    const auto healths         = make_device_health(state);

    ASSERT_EQ(healths.size(), 1u);
    const auto &m = healths[0].metrics();
    ASSERT_TRUE(m.count("missing") > 0);
    EXPECT_EQ(m.at("missing"), "true");
}

// ---------------------------------------------------------------------------
// populate_wait_ready
// ---------------------------------------------------------------------------

TEST(HealthTest, WaitReadyDegradedFalseWhenAllDevicesPresent) {
    const auto state = make_base_state();
    WaitReadyResponse response;
    populate_wait_ready(state, response);

    const auto &d = response.diagnostics();
    ASSERT_TRUE(d.count("degraded") > 0);
    EXPECT_EQ(d.at("degraded"), "false");
}

TEST(HealthTest, WaitReadyDegradedTrueWhenExpectedDevicesMissing) {
    auto state                 = make_base_state();
    state.missing_expected_ids = {"rlht-0x10"};
    WaitReadyResponse response;
    populate_wait_ready(state, response);

    const auto &d = response.diagnostics();
    ASSERT_TRUE(d.count("degraded") > 0);
    EXPECT_EQ(d.at("degraded"), "true");
}

TEST(HealthTest, WaitReadyDiagnosticsIncludesMissingExpectedCount) {
    auto state                 = make_base_state();
    state.missing_expected_ids = {"rlht-0x10", "dcmt-0x20"};
    WaitReadyResponse response;
    populate_wait_ready(state, response);

    const auto &d = response.diagnostics();
    ASSERT_TRUE(d.count("missing_expected_count") > 0);
    EXPECT_EQ(d.at("missing_expected_count"), "2");
}

} // namespace
} // namespace anolis_provider_bread::health
