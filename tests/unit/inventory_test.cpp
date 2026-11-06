#include "devices/common/inventory.hpp"

#include <gtest/gtest.h>

#include <string>

#include "config/provider_config.hpp"

namespace anolis_provider_bread {
namespace {

TEST(StubInventoryTest, BuildsSeedInventoryForConfiguredDevices) {
    ProviderConfig config;
    config.provider_name = "bread-lab";
    config.bus_path = "/dev/i2c-1";
    config.devices = {
        DeviceSpec{"rlht0", DeviceType::Rlht, "Left Heater", 0x08},
        DeviceSpec{"dcmt0", DeviceType::Dcmt, "Conveyor Drive", 0x09},
    };

    const auto inventory_devices = inventory::build_seed_inventory(config);
    ASSERT_EQ(inventory_devices.size(), 2U);

    EXPECT_EQ(inventory_devices[0].descriptor.device_id(), "rlht0");
    // [§5.1] Device.provider_name is the fixed ADPP identity, matching Hello —
    // NOT config.provider_name (set to "bread-lab" above), which could diverge.
    EXPECT_EQ(inventory_devices[0].descriptor.provider_name(), "anolis-provider-bread");
    EXPECT_EQ(inventory_devices[0].descriptor.type_id(), "bread.rlht");
    EXPECT_EQ(inventory_devices[0].descriptor.address(), "0x08");
    // One opaque claim per device, in the SDK's canonical form; the old
    // ownership tags are gone (anolis#318).
    EXPECT_EQ(inventory_devices[0].descriptor.tags().at("anolis.claim"), "i2c:/dev/i2c-1:0x08");
    EXPECT_EQ(inventory_devices[0].descriptor.tags().count("hw.bus_path"), 0U);
    EXPECT_EQ(inventory_devices[0].descriptor.tags().count("hw.i2c_address"), 0U);
    EXPECT_EQ(inventory_devices[0].descriptor.tags().count("bus_path"), 0U);
    EXPECT_EQ(inventory_devices[0].descriptor.tags().count("i2c_address"), 0U);
    EXPECT_EQ(inventory_devices[1].descriptor.type_id(), "bread.dcmt");

    EXPECT_EQ(inventory_devices[0].capabilities.functions_size(), 6);
    // The seeded DCMT advertises closed-loop speed, so it also gets set_speed_pid_x1000.
    EXPECT_EQ(inventory_devices[1].capabilities.functions_size(), 7);
    EXPECT_TRUE(inventory::signal_exists(inventory_devices[0], "t1_c"));
    EXPECT_TRUE(inventory::signal_exists(inventory_devices[1], "motor1_value"));
    EXPECT_TRUE(inventory::function_exists(inventory_devices[0], 1, ""));
    EXPECT_TRUE(inventory::function_exists(inventory_devices[1], 0, "set_mode"));

    const auto find_fn = [](const anolis::deviceprovider::v1::CapabilitySet &caps,
                            const std::string &name) -> const anolis::deviceprovider::v1::FunctionSpec * {
        for (const auto &fn : caps.functions()) {
            if (fn.name() == name) {
                return &fn;
            }
        }
        return nullptr;
    };

    const auto &rlht_caps = inventory_devices[0].capabilities;
    const auto *set_setpoints = find_fn(rlht_caps, "set_setpoints");
    ASSERT_NE(set_setpoints, nullptr);
    ASSERT_EQ(set_setpoints->args_size(), 2);
    EXPECT_EQ(set_setpoints->args(0).name(), "setpoint1_c");
    EXPECT_EQ(set_setpoints->args(0).min_double(), -3276.8);
    EXPECT_EQ(set_setpoints->args(0).max_double(), 3276.7);
    EXPECT_EQ(set_setpoints->args(1).name(), "setpoint2_c");
    EXPECT_EQ(set_setpoints->args(1).min_double(), -3276.8);
    EXPECT_EQ(set_setpoints->args(1).max_double(), 3276.7);

    const auto *set_open_duty = find_fn(rlht_caps, "set_open_duty_pct");
    ASSERT_NE(set_open_duty, nullptr);
    ASSERT_EQ(set_open_duty->args_size(), 2);
    EXPECT_EQ(set_open_duty->args(0).name(), "duty1_pct");
    EXPECT_EQ(set_open_duty->args(0).max_uint64(), 100u);
    EXPECT_EQ(set_open_duty->args(1).name(), "duty2_pct");
    EXPECT_EQ(set_open_duty->args(1).max_uint64(), 100u);

    // The range Slice_RLHT acts on; it clamps anything outside it (#162).
    const auto *set_periods = find_fn(rlht_caps, "set_periods_ms");
    ASSERT_NE(set_periods, nullptr);
    ASSERT_EQ(set_periods->args_size(), 2);
    EXPECT_EQ(set_periods->args(0).name(), "period1_ms");
    EXPECT_EQ(set_periods->args(0).min_uint64(), 100u);
    EXPECT_EQ(set_periods->args(0).max_uint64(), 10000u);
    EXPECT_EQ(set_periods->args(1).name(), "period2_ms");
    EXPECT_EQ(set_periods->args(1).min_uint64(), 100u);
    EXPECT_EQ(set_periods->args(1).max_uint64(), 10000u);

    const auto &dcmt_caps = inventory_devices[1].capabilities;
    const auto *set_open_loop = find_fn(dcmt_caps, "set_open_loop");
    ASSERT_NE(set_open_loop, nullptr);
    ASSERT_EQ(set_open_loop->args_size(), 2);
    EXPECT_EQ(set_open_loop->args(0).name(), "motor1_pwm");
    EXPECT_EQ(set_open_loop->args(0).min_int64(), -255);
    EXPECT_EQ(set_open_loop->args(0).max_int64(), 255);
    EXPECT_EQ(set_open_loop->args(1).name(), "motor2_pwm");
    EXPECT_EQ(set_open_loop->args(1).min_int64(), -255);
    EXPECT_EQ(set_open_loop->args(1).max_int64(), 255);

    const auto *set_setpoint = find_fn(dcmt_caps, "set_setpoint");
    ASSERT_NE(set_setpoint, nullptr);
    ASSERT_EQ(set_setpoint->args_size(), 2);
    EXPECT_EQ(set_setpoint->args(0).name(), "motor1_target");
    EXPECT_EQ(set_setpoint->args(0).min_int64(), -32768);
    EXPECT_EQ(set_setpoint->args(0).max_int64(), 32767);
    EXPECT_EQ(set_setpoint->args(1).name(), "motor2_target");
    EXPECT_EQ(set_setpoint->args(1).min_int64(), -32768);
    EXPECT_EQ(set_setpoint->args(1).max_int64(), 32767);
}

// set_tc_select chooses the thermocouple each heater channel follows, and
// Slice_RLHT refuses anything but 1 or 2 (#221). GET_STATE's on-times and
// selections are published as signals named to match (#222).
TEST(StubInventoryTest, RlhtTcSelectNamesChannelsAndGetStateFieldsAreSignals) {
    ProviderConfig config;
    config.bus_path = "/dev/i2c-1";
    config.devices = {DeviceSpec{"rlht0", DeviceType::Rlht, "Heater", 0x08}};

    const auto devices = inventory::build_seed_inventory(config);
    ASSERT_EQ(devices.size(), 1U);
    const auto &caps = devices[0].capabilities;

    const anolis::deviceprovider::v1::FunctionSpec *set_tc_select = nullptr;
    for (const auto &fn : caps.functions()) {
        if (fn.name() == "set_tc_select") {
            set_tc_select = &fn;
        }
    }
    ASSERT_NE(set_tc_select, nullptr);
    ASSERT_EQ(set_tc_select->args_size(), 2);
    EXPECT_EQ(set_tc_select->args(0).name(), "channel1_thermocouple");
    EXPECT_EQ(set_tc_select->args(1).name(), "channel2_thermocouple");
    for (const auto &arg : set_tc_select->args()) {
        SCOPED_TRACE(arg.name());
        EXPECT_EQ(arg.type(), anolis::deviceprovider::v1::VALUE_TYPE_UINT64);
        EXPECT_TRUE(arg.required());
        EXPECT_EQ(arg.min_uint64(), 1u);
        EXPECT_EQ(arg.max_uint64(), 2u);
    }

    const auto find_signal = [&](const std::string &id) -> const anolis::deviceprovider::v1::SignalSpec * {
        for (const auto &signal : caps.signals()) {
            if (signal.signal_id() == id) {
                return &signal;
            }
        }
        return nullptr;
    };
    for (const char *id : {"on1_ms", "on2_ms"}) {
        SCOPED_TRACE(id);
        const auto *signal = find_signal(id);
        ASSERT_NE(signal, nullptr);
        EXPECT_EQ(signal->value_type(), anolis::deviceprovider::v1::VALUE_TYPE_UINT64);
        EXPECT_EQ(signal->unit(), "ms");
    }
    for (const char *id : {"channel1_thermocouple", "channel2_thermocouple"}) {
        SCOPED_TRACE(id);
        const auto *signal = find_signal(id);
        ASSERT_NE(signal, nullptr);
        EXPECT_EQ(signal->value_type(), anolis::deviceprovider::v1::VALUE_TYPE_UINT64);
        EXPECT_TRUE(signal->unit().empty());
    }
}

}  // namespace
}  // namespace anolis_provider_bread
