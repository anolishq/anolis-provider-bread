/**
 * @file inventory.cpp
 * @brief Capability and inventory assembly logic for discovered or seeded BREAD
 * devices.
 *
 * This layer converts raw probe results into stable ADPP device descriptors and
 * capability sets, including fallback behavior when capability discovery is not
 * available.
 */

#include "devices/common/inventory.hpp"

#include <algorithm>
#include <climits>
#include <iomanip>
#include <optional>
#include <sstream>
#include <tuple>

extern "C" {
#include <bread/bread_caps.h>
#include <bread/dcmt_ops.h>
#include <bread/rlht_ops.h>
#include <crumbs_version.h>
}

namespace anolis_provider_bread::inventory {
namespace {

using ArgSpec = anolis::deviceprovider::v1::ArgSpec;
using FunctionSpec = anolis::deviceprovider::v1::FunctionSpec;
using SignalSpec = anolis::deviceprovider::v1::SignalSpec;

constexpr auto VT_BOOL = anolis::deviceprovider::v1::VALUE_TYPE_BOOL;
constexpr auto VT_INT64 = anolis::deviceprovider::v1::VALUE_TYPE_INT64;
constexpr auto VT_UINT64 = anolis::deviceprovider::v1::VALUE_TYPE_UINT64;
constexpr auto VT_DOUBLE = anolis::deviceprovider::v1::VALUE_TYPE_DOUBLE;
constexpr auto VT_STRING = anolis::deviceprovider::v1::VALUE_TYPE_STRING;

constexpr auto CAT_CONFIG =
    anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_CONFIG;
constexpr auto CAT_ACTUATE =
    anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_ACTUATE;

std::string format_u32_hex(uint32_t value) {
  std::ostringstream out;
  out << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
      << value;
  return out.str();
}

std::string format_module_version(const ModuleVersion &version) {
  std::ostringstream out;
  out << static_cast<int>(version.module_major) << '.'
      << static_cast<int>(version.module_minor) << '.'
      << static_cast<int>(version.module_patch);
  return out.str();
}

std::string format_crumbs_version(uint16_t value) {
  std::ostringstream out;
  out << (value / 10000u) << '.' << ((value / 100u) % 100u) << '.'
      << (value % 100u);
  return out.str();
}

ArgSpec *add_arg(FunctionSpec &function, const std::string &name,
                 anolis::deviceprovider::v1::ValueType type,
                 const std::string &description, bool required,
                 const std::string &unit = "") {
  ArgSpec *arg = function.add_args();
  arg->set_name(name);
  arg->set_type(type);
  arg->set_description(description);
  arg->set_required(required);
  if (!unit.empty()) {
    arg->set_unit(unit);
  }
  return arg;
}

FunctionSpec *
add_function(CapabilitySet &caps, uint32_t function_id, const std::string &name,
             const std::string &description,
             anolis::deviceprovider::v1::FunctionPolicy_Category category) {
  FunctionSpec *function = caps.add_functions();
  function->set_function_id(function_id);
  function->set_name(name);
  function->set_description(description);
  function->mutable_policy()->set_category(category);
  function->mutable_policy()->set_is_idempotent(false);
  return function;
}

void add_signal(CapabilitySet &caps, const std::string &signal_id,
                const std::string &description,
                anolis::deviceprovider::v1::ValueType type,
                const std::string &unit = "") {
  SignalSpec *signal = caps.add_signals();
  signal->set_signal_id(signal_id);
  signal->set_name(signal_id);
  signal->set_description(description);
  signal->set_value_type(type);
  signal->set_poll_hint_hz(1.0);
  signal->set_stale_after_ms(5000);
  if (!unit.empty()) {
    signal->set_unit(unit);
  }
}

CapabilitySet build_rlht_capabilities(uint32_t flags) {
  CapabilitySet caps;

  if ((flags & RLHT_CAP_MODE_CONTROL) != 0u) {
    auto *set_mode =
        add_function(caps, 1, "set_mode", "Set RLHT control mode.", CAT_CONFIG);
    add_arg(*set_mode, "mode", VT_STRING, "One of closed_loop or open_loop.",
            true);
  }

  if ((flags & RLHT_CAP_SETPOINT_CONTROL) != 0u) {
    auto *set_setpoints =
        add_function(caps, 2, "set_setpoints",
                     "Set RLHT channel temperature setpoints.", CAT_CONFIG);
    auto *sp1_arg = add_arg(*set_setpoints, "setpoint1_c", VT_DOUBLE,
                            "Channel 1 target temperature.", true, "C");
    sp1_arg->set_min_double(-3276.8);
    sp1_arg->set_max_double(3276.7);
    auto *sp2_arg = add_arg(*set_setpoints, "setpoint2_c", VT_DOUBLE,
                            "Channel 2 target temperature.", true, "C");
    sp2_arg->set_min_double(-3276.8);
    sp2_arg->set_max_double(3276.7);
  }

  if ((flags & RLHT_CAP_PID_TUNING) != 0u) {
    auto *set_pid = add_function(caps, 3, "set_pid_x10",
                                 "Set RLHT PID gains encoded x10.", CAT_CONFIG);
    auto *kp1_arg = add_arg(*set_pid, "kp1_x10", VT_UINT64,
                            "Channel 1 proportional gain x10.", true);
    kp1_arg->set_min_uint64(0);
    kp1_arg->set_max_uint64(255);
    auto *ki1_arg = add_arg(*set_pid, "ki1_x10", VT_UINT64,
                            "Channel 1 integral gain x10.", true);
    ki1_arg->set_min_uint64(0);
    ki1_arg->set_max_uint64(255);
    auto *kd1_arg = add_arg(*set_pid, "kd1_x10", VT_UINT64,
                            "Channel 1 derivative gain x10.", true);
    kd1_arg->set_min_uint64(0);
    kd1_arg->set_max_uint64(255);
    auto *kp2_arg = add_arg(*set_pid, "kp2_x10", VT_UINT64,
                            "Channel 2 proportional gain x10.", true);
    kp2_arg->set_min_uint64(0);
    kp2_arg->set_max_uint64(255);
    auto *ki2_arg = add_arg(*set_pid, "ki2_x10", VT_UINT64,
                            "Channel 2 integral gain x10.", true);
    ki2_arg->set_min_uint64(0);
    ki2_arg->set_max_uint64(255);
    auto *kd2_arg = add_arg(*set_pid, "kd2_x10", VT_UINT64,
                            "Channel 2 derivative gain x10.", true);
    kd2_arg->set_min_uint64(0);
    kd2_arg->set_max_uint64(255);
  }

  if ((flags & RLHT_CAP_PERIOD_CONTROL) != 0u) {
    auto *set_periods = add_function(caps, 4, "set_periods_ms",
                                     "Set RLHT relay periods.", CAT_CONFIG);
    auto *p1_arg = add_arg(*set_periods, "period1_ms", VT_UINT64,
                           "Relay 1 period.", true, "ms");
    p1_arg->set_min_uint64(0);
    p1_arg->set_max_uint64(65535);
    auto *p2_arg = add_arg(*set_periods, "period2_ms", VT_UINT64,
                           "Relay 2 period.", true, "ms");
    p2_arg->set_min_uint64(0);
    p2_arg->set_max_uint64(65535);
  }

  if ((flags & RLHT_CAP_TC_SELECT) != 0u) {
    auto *set_tc_select =
        add_function(caps, 5, "set_tc_select",
                     "Select RLHT thermocouple inputs.", CAT_CONFIG);
    auto *tc1_arg = add_arg(*set_tc_select, "tc1_index", VT_UINT64,
                            "Channel 1 thermocouple index.", true);
    tc1_arg->set_min_uint64(0);
    tc1_arg->set_max_uint64(255);
    auto *tc2_arg = add_arg(*set_tc_select, "tc2_index", VT_UINT64,
                            "Channel 2 thermocouple index.", true);
    tc2_arg->set_min_uint64(0);
    tc2_arg->set_max_uint64(255);
  }

  if ((flags & RLHT_CAP_OPEN_DUTY_CONTROL) != 0u) {
    auto *set_open_duty =
        add_function(caps, 6, "set_open_duty_pct",
                     "Set RLHT open-loop duty percentages.", CAT_ACTUATE);
    auto *d1_arg = add_arg(*set_open_duty, "duty1_pct", VT_UINT64,
                           "Channel 1 duty cycle percentage.", true, "%");
    d1_arg->set_min_uint64(0);
    d1_arg->set_max_uint64(100);
    auto *d2_arg = add_arg(*set_open_duty, "duty2_pct", VT_UINT64,
                           "Channel 2 duty cycle percentage.", true, "%");
    d2_arg->set_min_uint64(0);
    d2_arg->set_max_uint64(100);
  }

  add_signal(caps, "mode", "Current RLHT control mode.", VT_STRING);
  add_signal(caps, "t1_c", "Channel 1 measured temperature.", VT_DOUBLE, "C");
  add_signal(caps, "t2_c", "Channel 2 measured temperature.", VT_DOUBLE, "C");
  add_signal(caps, "setpoint1_c", "Channel 1 target temperature.", VT_DOUBLE,
             "C");
  add_signal(caps, "setpoint2_c", "Channel 2 target temperature.", VT_DOUBLE,
             "C");
  add_signal(caps, "period1_ms", "Relay 1 control period.", VT_UINT64, "ms");
  add_signal(caps, "period2_ms", "Relay 2 control period.", VT_UINT64, "ms");
  add_signal(caps, "relay1_on", "Relay 1 state derived from RLHT flags.",
             VT_BOOL);
  add_signal(caps, "relay2_on", "Relay 2 state derived from RLHT flags.",
             VT_BOOL);
  add_signal(caps, "estop", "Emergency stop state derived from RLHT flags.",
             VT_BOOL);

  return caps;
}

CapabilitySet build_dcmt_capabilities(uint32_t flags) {
  CapabilitySet caps;

  if ((flags & DCMT_CAP_OPEN_LOOP_CONTROL) != 0u) {
    auto *set_open_loop =
        add_function(caps, 1, "set_open_loop", "Set DCMT open-loop PWM values.",
                     CAT_ACTUATE);
    auto *m1_arg = add_arg(*set_open_loop, "motor1_pwm", VT_INT64,
                           "Motor 1 PWM command.", true);
    m1_arg->set_min_int64(-255);
    m1_arg->set_max_int64(255);
    auto *m2_arg = add_arg(*set_open_loop, "motor2_pwm", VT_INT64,
                           "Motor 2 PWM command.", true);
    m2_arg->set_min_int64(-255);
    m2_arg->set_max_int64(255);
  }

  if ((flags & DCMT_CAP_BRAKE_CONTROL) != 0u) {
    auto *set_brake = add_function(caps, 2, "set_brake",
                                   "Set DCMT brake outputs.", CAT_ACTUATE);
    add_arg(*set_brake, "motor1_brake", VT_BOOL, "Motor 1 brake state.", true);
    add_arg(*set_brake, "motor2_brake", VT_BOOL, "Motor 2 brake state.", true);
  }

  if ((flags & (DCMT_CAP_OPEN_LOOP_CONTROL | DCMT_CAP_CLOSED_LOOP_POSITION |
                DCMT_CAP_CLOSED_LOOP_SPEED)) != 0u) {
    auto *set_mode =
        add_function(caps, 3, "set_mode", "Set DCMT control mode.", CAT_CONFIG);
    add_arg(*set_mode, "mode", VT_STRING,
            "One of open_loop, closed_position, or closed_speed.", true);
  }

  if ((flags & (DCMT_CAP_CLOSED_LOOP_POSITION | DCMT_CAP_CLOSED_LOOP_SPEED)) !=
      0u) {
    auto *set_setpoint = add_function(caps, 4, "set_setpoint",
                                      "Set DCMT control targets.", CAT_CONFIG);
    auto *t1_arg = add_arg(*set_setpoint, "motor1_target", VT_INT64,
                           "Motor 1 target value.", true);
    t1_arg->set_min_int64(INT16_MIN);
    t1_arg->set_max_int64(INT16_MAX);
    auto *t2_arg = add_arg(*set_setpoint, "motor2_target", VT_INT64,
                           "Motor 2 target value.", true);
    t2_arg->set_min_int64(INT16_MIN);
    t2_arg->set_max_int64(INT16_MAX);
  }

  if ((flags & DCMT_CAP_PID_TUNING) != 0u) {
    auto *set_pid = add_function(caps, 5, "set_pid_x10",
                                 "Set DCMT PID gains encoded x10.", CAT_CONFIG);
    auto *kp1_arg = add_arg(*set_pid, "kp1_x10", VT_UINT64,
                            "Motor 1 proportional gain x10.", true);
    kp1_arg->set_min_uint64(0);
    kp1_arg->set_max_uint64(255);
    auto *ki1_arg = add_arg(*set_pid, "ki1_x10", VT_UINT64,
                            "Motor 1 integral gain x10.", true);
    ki1_arg->set_min_uint64(0);
    ki1_arg->set_max_uint64(255);
    auto *kd1_arg = add_arg(*set_pid, "kd1_x10", VT_UINT64,
                            "Motor 1 derivative gain x10.", true);
    kd1_arg->set_min_uint64(0);
    kd1_arg->set_max_uint64(255);
    auto *kp2_arg = add_arg(*set_pid, "kp2_x10", VT_UINT64,
                            "Motor 2 proportional gain x10.", true);
    kp2_arg->set_min_uint64(0);
    kp2_arg->set_max_uint64(255);
    auto *ki2_arg = add_arg(*set_pid, "ki2_x10", VT_UINT64,
                            "Motor 2 integral gain x10.", true);
    ki2_arg->set_min_uint64(0);
    ki2_arg->set_max_uint64(255);
    auto *kd2_arg = add_arg(*set_pid, "kd2_x10", VT_UINT64,
                            "Motor 2 derivative gain x10.", true);
    kd2_arg->set_min_uint64(0);
    kd2_arg->set_max_uint64(255);
  }

  add_signal(caps, "mode", "Current DCMT control mode.", VT_STRING);
  add_signal(caps, "motor1_target", "Motor 1 target value.", VT_INT64);
  add_signal(caps, "motor2_target", "Motor 2 target value.", VT_INT64);
  add_signal(caps, "motor1_value", "Motor 1 measured value.", VT_INT64);
  add_signal(caps, "motor2_value", "Motor 2 measured value.", VT_INT64);
  add_signal(caps, "motor1_brake", "Motor 1 brake state.", VT_BOOL);
  add_signal(caps, "motor2_brake", "Motor 2 brake state.", VT_BOOL);
  add_signal(caps, "estop", "Emergency stop state.", VT_BOOL);

  return caps;
}

CapabilitySet build_capabilities(DeviceType type, uint32_t flags) {
  switch (type) {
  case DeviceType::Rlht:
    return build_rlht_capabilities(flags);
  case DeviceType::Dcmt:
    return build_dcmt_capabilities(flags);
  }

  return CapabilitySet{};
}

std::string generated_label(DeviceType type, int address) {
  const std::string type_name = type == DeviceType::Rlht ? "RLHT" : "DCMT";
  return type_name + " " + format_i2c_address(address);
}

std::string generated_id(DeviceType type, int index) {
  const std::string prefix = type == DeviceType::Rlht ? "rlht" : "dcmt";
  return prefix + std::to_string(index);
}

CapabilityProfile normalize_capability_profile(DeviceType type,
                                               CapabilityProfile profile) {
  if (profile.source == CapabilitySource::BaselineFallback) {
    const CapabilityProfile baseline = make_baseline_capability_profile(type);
    if (profile.schema == 0) {
      profile.schema = baseline.schema;
    }
    if (profile.level == 0) {
      profile.level = baseline.level;
    }
    if (profile.flags == 0) {
      profile.flags = baseline.flags;
    }
  }
  if (profile.source == CapabilitySource::Seeded) {
    const CapabilityProfile seeded = make_seeded_capability_profile(type);
    if (profile.schema == 0) {
      profile.schema = seeded.schema;
    }
    if (profile.level == 0) {
      profile.level = seeded.level;
    }
    if (profile.flags == 0) {
      profile.flags = seeded.flags;
    }
  }
  return profile;
}

std::optional<std::size_t>
find_config_match_by_address(const ProviderConfig &config, int address) {
  for (std::size_t i = 0; i < config.devices.size(); ++i) {
    if (config.devices[i].address == address) {
      return i;
    }
  }
  return std::nullopt;
}

InventoryDevice build_device(const ProviderConfig &config,
                             const ProbeRecord &probe, DeviceType type,
                             InventorySource source, bool expected,
                             const std::string &device_id,
                             const std::string &label) {
  InventoryDevice device;
  const std::string formatted_address = format_i2c_address(probe.address);
  device.type = type;
  device.address = probe.address;
  device.source = source;
  device.expected = expected;
  device.version = probe.version;
  device.capability_profile =
      normalize_capability_profile(type, probe.capability_profile);

  device.descriptor.set_device_id(device_id);
  device.descriptor.set_provider_name(config.provider_name);
  device.descriptor.set_label(label);
  device.descriptor.set_address(formatted_address);
  device.descriptor.set_type_id(provider_type_id(type));
  device.descriptor.set_type_version(
      std::to_string(device.version.module_major == 0
                         ? 1
                         : static_cast<int>(device.version.module_major)));
  device.descriptor.mutable_tags()->insert({"hw.bus_path", config.bus_path});
  device.descriptor.mutable_tags()->insert(
      {"hw.i2c_address", formatted_address});
  device.descriptor.mutable_tags()->insert({"bus_path", config.bus_path});
  device.descriptor.mutable_tags()->insert({"i2c_address", formatted_address});
  device.descriptor.mutable_tags()->insert({"family", "bread"});
  device.descriptor.mutable_tags()->insert({"inventory", to_string(source)});
  device.descriptor.mutable_tags()->insert(
      {"contract", bread_contract_name(type)});
  device.descriptor.mutable_tags()->insert(
      {"caps_source", to_string(device.capability_profile.source)});
  device.descriptor.mutable_tags()->insert(
      {"caps_level", std::to_string(device.capability_profile.level)});
  device.descriptor.mutable_tags()->insert(
      {"caps_flags", format_u32_hex(device.capability_profile.flags)});
  device.descriptor.mutable_tags()->insert(
      {"expected", expected ? "true" : "false"});
  device.descriptor.mutable_tags()->insert({"compatibility", "supported"});
  device.descriptor.mutable_tags()->insert(
      {"crumbs_version", format_crumbs_version(device.version.crumbs_version)});
  device.descriptor.mutable_tags()->insert(
      {"module_version", format_module_version(device.version)});

  device.capabilities =
      build_capabilities(type, device.capability_profile.flags);
  return device;
}

} // namespace

CapabilityProfile make_baseline_capability_profile(DeviceType type) {
  CapabilityProfile profile;
  profile.schema = BREAD_CAPS_SCHEMA_V1;
  profile.source = CapabilitySource::BaselineFallback;

  switch (type) {
  case DeviceType::Rlht:
    profile.level = RLHT_CAP_LEVEL_1;
    profile.flags = RLHT_CAP_BASELINE_FLAGS;
    break;
  case DeviceType::Dcmt:
    profile.level = DCMT_CAP_LEVEL_1;
    profile.flags = DCMT_CAP_BASELINE_FLAGS;
    break;
  }

  return profile;
}

CapabilityProfile make_seeded_capability_profile(DeviceType type) {
  CapabilityProfile profile;
  profile.schema = BREAD_CAPS_SCHEMA_V1;
  profile.source = CapabilitySource::Seeded;

  switch (type) {
  case DeviceType::Rlht:
    profile.level = RLHT_CAP_LEVEL_1;
    profile.flags = RLHT_CAP_BASELINE_FLAGS;
    break;
  case DeviceType::Dcmt:
    profile.level = DCMT_CAP_LEVEL_3;
    profile.flags = DCMT_CAP_OPEN_LOOP_CONTROL | DCMT_CAP_BRAKE_CONTROL |
                    DCMT_CAP_CLOSED_LOOP_POSITION | DCMT_CAP_CLOSED_LOOP_SPEED |
                    DCMT_CAP_PID_TUNING;
    break;
  }

  return profile;
}

std::vector<ProbeRecord> build_seed_probes(const ProviderConfig &config) {
  std::vector<ProbeRecord> probes;
  probes.reserve(config.devices.size());

  for (const DeviceSpec &spec : config.devices) {
    ProbeRecord probe;
    probe.address = spec.address;
    probe.type_id = bread_type_id(spec.type);
    probe.status = ProbeStatus::Supported;
    probe.version.crumbs_version = CRUMBS_VERSION;
    switch (spec.type) {
    case DeviceType::Rlht:
      probe.version.module_major = RLHT_MODULE_VER_MAJOR;
      probe.version.module_minor = RLHT_MODULE_VER_MINOR;
      probe.version.module_patch = RLHT_MODULE_VER_PATCH;
      break;
    case DeviceType::Dcmt:
      probe.version.module_major = DCMT_MODULE_VER_MAJOR;
      probe.version.module_minor = DCMT_MODULE_VER_MINOR;
      probe.version.module_patch = DCMT_MODULE_VER_PATCH;
      break;
    }
    probe.capability_profile = make_seeded_capability_profile(spec.type);
    probe.detail = "config-seeded";
    probes.push_back(probe);
  }

  return probes;
}

InventoryBuildResult
build_inventory_from_probes(const ProviderConfig &config,
                            const std::vector<ProbeRecord> &probes,
                            InventorySource source) {
  std::vector<ProbeRecord> sorted_probes = probes;
  std::sort(sorted_probes.begin(), sorted_probes.end(),
            [](const ProbeRecord &lhs, const ProbeRecord &rhs) {
              return std::tie(lhs.address, lhs.type_id) <
                     std::tie(rhs.address, rhs.type_id);
            });

  InventoryBuildResult result;
  std::vector<bool> matched(config.devices.size(), false);
  int rlht_index = 0;
  int dcmt_index = 0;

  for (ProbeRecord probe : sorted_probes) {
    DeviceType type = DeviceType::Rlht;
    if (probe.status != ProbeStatus::Supported) {
      result.unsupported_probes.push_back(std::move(probe));
      continue;
    }
    if (!try_parse_bread_type(probe.type_id, type)) {
      probe.status = ProbeStatus::UnsupportedType;
      if (probe.detail.empty()) {
        probe.detail = "unsupported BREAD type id";
      }
      result.unsupported_probes.push_back(std::move(probe));
      continue;
    }

    // Matching is address-first: a configured entry at the same address
    // provides the stable device ID/label and marks the device as expected.
    // Unmatched supported probes still become inventory entries with
    // generated IDs so scan mode can surface extra hardware.
    const auto match_index =
        find_config_match_by_address(config, probe.address);
    bool expected = false;
    std::string device_id;
    std::string label;

    int *type_counter = type == DeviceType::Rlht ? &rlht_index : &dcmt_index;

    if (match_index.has_value()) {
      const DeviceSpec &spec = config.devices[*match_index];
      if (spec.type != type) {
        probe.status = ProbeStatus::TypeMismatch;
        probe.detail = "configured type '" + to_string(spec.type) +
                       "' does not match probed type '" + to_string(type) + "'";
        result.unsupported_probes.push_back(std::move(probe));
        continue;
      }
      expected = true;
      matched[*match_index] = true;
      device_id = spec.id;
      label = spec.label.empty() ? generated_label(type, probe.address)
                                 : spec.label;
      *type_counter += 1;
    } else {
      const int generated_index = *type_counter;
      *type_counter += 1;
      device_id = generated_id(type, generated_index);
      label = generated_label(type, probe.address);
    }

    probe.capability_profile =
        normalize_capability_profile(type, probe.capability_profile);
    result.supported_devices.push_back(
        build_device(config, probe, type, source, expected, device_id, label));
  }

  for (std::size_t i = 0; i < config.devices.size(); ++i) {
    if (!matched[i]) {
      result.missing_expected_ids.push_back(config.devices[i].id);
    }
  }

  return result;
}

std::vector<InventoryDevice>
build_seed_inventory(const ProviderConfig &config) {
  // No-hardware builds intentionally reuse the same probe-to-inventory path
  // so seeded and live inventory behave as similarly as possible.
  return build_inventory_from_probes(config, build_seed_probes(config),
                                     InventorySource::ConfigSeeded)
      .supported_devices;
}

std::string to_string(InventorySource source) {
  switch (source) {
  case InventorySource::ConfigSeeded:
    return "config_seeded";
  case InventorySource::Manual:
    return "manual";
  case InventorySource::Discovered:
    return "discovered";
  }

  return "unknown";
}

std::string to_string(CapabilitySource source) {
  switch (source) {
  case CapabilitySource::Seeded:
    return "seeded";
  case CapabilitySource::Queried:
    return "queried";
  case CapabilitySource::BaselineFallback:
    return "baseline_fallback";
  }

  return "unknown";
}

const InventoryDevice *find_device(const std::vector<InventoryDevice> &devices,
                                   const std::string &device_id) {
  for (const InventoryDevice &device : devices) {
    if (device.descriptor.device_id() == device_id) {
      return &device;
    }
  }
  return nullptr;
}

bool signal_exists(const InventoryDevice &device,
                   const std::string &signal_id) {
  for (const auto &signal : device.capabilities.signals()) {
    if (signal.signal_id() == signal_id) {
      return true;
    }
  }
  return false;
}

bool function_exists(const InventoryDevice &device, uint32_t function_id,
                     const std::string &function_name) {
  for (const auto &function : device.capabilities.functions()) {
    const bool id_match =
        function_id != 0 && function.function_id() == function_id;
    const bool name_match =
        !function_name.empty() && function.name() == function_name;
    if (id_match || name_match) {
      return true;
    }
  }
  return false;
}

} // namespace anolis_provider_bread::inventory
