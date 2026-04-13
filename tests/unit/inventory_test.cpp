#include "config/provider_config.hpp"
#include "devices/common/inventory.hpp"

#include <gtest/gtest.h>

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
  EXPECT_EQ(inventory_devices[0].descriptor.type_id(), "bread.rlht");
  EXPECT_EQ(inventory_devices[0].descriptor.address(), "0x08");
  EXPECT_EQ(inventory_devices[0].descriptor.tags().at("hw.bus_path"),
            "/dev/i2c-1");
  EXPECT_EQ(inventory_devices[0].descriptor.tags().at("hw.i2c_address"),
            "0x08");
  EXPECT_EQ(inventory_devices[1].descriptor.type_id(), "bread.dcmt");

  EXPECT_EQ(inventory_devices[0].capabilities.functions_size(), 6);
  EXPECT_EQ(inventory_devices[1].capabilities.functions_size(), 5);
  EXPECT_TRUE(inventory::signal_exists(inventory_devices[0], "t1_c"));
  EXPECT_TRUE(inventory::signal_exists(inventory_devices[1], "motor1_value"));
  EXPECT_TRUE(inventory::function_exists(inventory_devices[0], 1, ""));
  EXPECT_TRUE(inventory::function_exists(inventory_devices[1], 0, "set_mode"));

  const auto find_fn =
      [](const anolis::deviceprovider::v1::CapabilitySet &caps,
         const std::string &name)
      -> const anolis::deviceprovider::v1::FunctionSpec * {
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

} // namespace
} // namespace anolis_provider_bread
