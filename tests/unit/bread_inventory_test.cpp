#include <gtest/gtest.h>

#include <map>
#include <string>

#include "devices/common/inventory.hpp"

extern "C" {
#include <bread/bread_caps.h>
#include <bread/dcmt_ops.h>
#include <bread/rlht_ops.h>
#include <crumbs_version.h>
}

namespace anolis_provider_bread::inventory {
namespace {

ProviderConfig make_base_config() {
    ProviderConfig config;
    config.provider_name = "bread-lab";
    config.bus_path = "/dev/i2c-1";
    return config;
}

TEST(BreadInventoryTest, VersionCompatibilityUsesBreadContractRules) {
    ModuleVersion compatible;
    compatible.crumbs_version = CRUMBS_VERSION;
    compatible.module_major = RLHT_MODULE_VER_MAJOR;
    compatible.module_minor = RLHT_MODULE_VER_MINOR;
    compatible.module_patch = RLHT_MODULE_VER_PATCH;

    std::string detail;
    EXPECT_EQ(evaluate_version_compatibility(RLHT_TYPE_ID, compatible, &detail), ProbeStatus::Supported);
    EXPECT_EQ(detail, "compatible");

    ModuleVersion bad_major = compatible;
    bad_major.module_major = 9;
    EXPECT_EQ(evaluate_version_compatibility(DCMT_TYPE_ID, bad_major, &detail), ProbeStatus::IncompatibleModuleMajor);

    ModuleVersion bad_crumbs = compatible;
    bad_crumbs.crumbs_version = 1;
    EXPECT_EQ(evaluate_version_compatibility(RLHT_TYPE_ID, bad_crumbs, &detail),
              ProbeStatus::IncompatibleCrumbsVersion);

    EXPECT_EQ(evaluate_version_compatibility(0x55u, compatible, &detail), ProbeStatus::UnsupportedType);
}

TEST(BreadInventoryTest, DiscoveryInventoryAssignsStableGeneratedIdsByTypeAndAddress) {
    ProviderConfig config = make_base_config();
    config.discovery_mode = DiscoveryMode::Scan;

    std::vector<ProbeRecord> probes = {
        ProbeRecord{0x09,
                    DCMT_TYPE_ID,
                    ProbeStatus::Supported,
                    {CRUMBS_VERSION, 1, 0, 0},
                    make_seeded_capability_profile(DeviceType::Dcmt),
                    "ok"},
        ProbeRecord{0x08,
                    RLHT_TYPE_ID,
                    ProbeStatus::Supported,
                    {CRUMBS_VERSION, 1, 0, 0},
                    make_seeded_capability_profile(DeviceType::Rlht),
                    "ok"},
        ProbeRecord{0x0A,
                    RLHT_TYPE_ID,
                    ProbeStatus::Supported,
                    {CRUMBS_VERSION, 1, 0, 0},
                    make_seeded_capability_profile(DeviceType::Rlht),
                    "ok"},
    };

    const InventoryBuildResult result = build_inventory_from_probes(config, probes, InventorySource::Discovered);
    ASSERT_EQ(result.supported_devices.size(), 3U);
    EXPECT_TRUE(result.unsupported_probes.empty());
    EXPECT_TRUE(result.missing_expected_ids.empty());

    EXPECT_EQ(result.supported_devices[0].descriptor.device_id(), "rlht0");
    EXPECT_EQ(result.supported_devices[1].descriptor.device_id(), "dcmt0");
    EXPECT_EQ(result.supported_devices[2].descriptor.device_id(), "rlht1");
    EXPECT_EQ(result.supported_devices[0].descriptor.tags().at("inventory"), "discovered");
    // One opaque claim per device, in the SDK's canonical form; the old
    // ownership tags are gone (anolis#318).
    EXPECT_EQ(result.supported_devices[0].descriptor.tags().at("anolis.claim"), "i2c:/dev/i2c-1:0x08");
    EXPECT_EQ(result.supported_devices[0].descriptor.tags().count("hw.bus_path"), 0U);
    EXPECT_EQ(result.supported_devices[0].descriptor.tags().count("hw.i2c_address"), 0U);
    EXPECT_EQ(result.supported_devices[0].descriptor.tags().count("bus_path"), 0U);
    EXPECT_EQ(result.supported_devices[0].descriptor.tags().count("i2c_address"), 0U);
}

TEST(BreadInventoryTest, ManualInventoryPreservesConfiguredIdentityAndTracksMissingExpected) {
    ProviderConfig config = make_base_config();
    config.discovery_mode = DiscoveryMode::Manual;
    config.manual_addresses = {0x08, 0x09};
    config.devices = {
        DeviceSpec{"reactor-left", DeviceType::Rlht, "Left Heater", 0x08},
        DeviceSpec{"drive-main", DeviceType::Dcmt, "Main Drive", 0x09},
    };

    std::vector<ProbeRecord> probes = {
        ProbeRecord{0x08,
                    RLHT_TYPE_ID,
                    ProbeStatus::Supported,
                    {CRUMBS_VERSION, 1, 0, 0},
                    make_seeded_capability_profile(DeviceType::Rlht),
                    "ok"},
    };

    const InventoryBuildResult result = build_inventory_from_probes(config, probes, InventorySource::Manual);
    ASSERT_EQ(result.supported_devices.size(), 1U);
    EXPECT_EQ(result.supported_devices[0].descriptor.device_id(), "reactor-left");
    EXPECT_EQ(result.supported_devices[0].descriptor.label(), "Left Heater");
    EXPECT_EQ(result.supported_devices[0].descriptor.tags().at("expected"), "true");
    ASSERT_EQ(result.missing_expected_ids.size(), 1U);
    EXPECT_EQ(result.missing_expected_ids[0], "drive-main");
}

TEST(BreadInventoryTest, MatchedConfigCarriesCommandWatchdogMsIntoInventory) {
    ProviderConfig config = make_base_config();
    config.discovery_mode = DiscoveryMode::Manual;
    config.manual_addresses = {0x14, 0x15};
    DeviceSpec configured{"drive-main", DeviceType::Dcmt, "Main Drive", 0x14};
    configured.command_watchdog_ms = 5000;
    config.devices = {configured};

    std::vector<ProbeRecord> probes = {
        ProbeRecord{0x14,
                    DCMT_TYPE_ID,
                    ProbeStatus::Supported,
                    {CRUMBS_VERSION, 1, 0, 0},
                    make_seeded_capability_profile(DeviceType::Dcmt),
                    "ok"},
        // Unmatched probe at 0x15: no config entry, watchdog stays 0.
        ProbeRecord{0x15,
                    DCMT_TYPE_ID,
                    ProbeStatus::Supported,
                    {CRUMBS_VERSION, 1, 0, 0},
                    make_seeded_capability_profile(DeviceType::Dcmt),
                    "ok"},
    };

    const InventoryBuildResult result = build_inventory_from_probes(config, probes, InventorySource::Manual);
    ASSERT_EQ(result.supported_devices.size(), 2U);
    for (const auto &device : result.supported_devices) {
        if (device.descriptor.device_id() == "drive-main") {
            EXPECT_EQ(device.command_watchdog_ms, 5000);
        } else {
            EXPECT_EQ(device.command_watchdog_ms, 0);
        }
    }
}

TEST(BreadInventoryTest, ReportedModuleVersionComesFromTheProbeNeverFromSeeding) {
    ProviderConfig config = make_base_config();
    config.discovery_mode = DiscoveryMode::Manual;
    config.manual_addresses = {0x08};
    config.devices = {DeviceSpec{"rlht0", DeviceType::Rlht, "Left Heater", 0x08}};

    const std::vector<ProbeRecord> probes = {
        ProbeRecord{0x08,
                    RLHT_TYPE_ID,
                    ProbeStatus::Supported,
                    {CRUMBS_VERSION, 1, 2, 3},
                    make_seeded_capability_profile(DeviceType::Rlht),
                    "probed"},
    };
    const InventoryBuildResult probed = build_inventory_from_probes(config, probes, InventorySource::Manual);
    ASSERT_EQ(probed.supported_devices.size(), 1U);
    std::map<std::string, std::string> probed_metrics;
    add_module_version_metric(probed.supported_devices[0], probed_metrics);
    EXPECT_EQ(probed_metrics, (std::map<std::string, std::string>{{"module_version", "1.2.3"}}));

    // A seeded version is this build's expectation, not a device reply.
    const std::vector<InventoryDevice> seeded = build_seed_inventory(config);
    ASSERT_EQ(seeded.size(), 1U);
    std::map<std::string, std::string> seeded_metrics;
    add_module_version_metric(seeded[0], seeded_metrics);
    EXPECT_TRUE(seeded_metrics.empty());
}

TEST(BreadInventoryTest, BaselineFallbackCapsGateDcmtFunctionsAndTypeMismatchIsUnsupported) {
    ProviderConfig config = make_base_config();
    config.discovery_mode = DiscoveryMode::Manual;
    config.manual_addresses = {0x08};
    config.devices = {
        DeviceSpec{"heater0", DeviceType::Rlht, "Heater", 0x08},
    };

    ProbeRecord dcmt_baseline;
    dcmt_baseline.address = 0x09;
    dcmt_baseline.type_id = DCMT_TYPE_ID;
    dcmt_baseline.status = ProbeStatus::Supported;
    dcmt_baseline.version = {CRUMBS_VERSION, 1, 0, 0};
    dcmt_baseline.capability_profile = make_baseline_capability_profile(DeviceType::Dcmt);
    dcmt_baseline.detail = "caps fallback";

    ProbeRecord mismatch;
    mismatch.address = 0x08;
    mismatch.type_id = DCMT_TYPE_ID;
    mismatch.status = ProbeStatus::Supported;
    mismatch.version = {CRUMBS_VERSION, 1, 0, 0};
    mismatch.capability_profile = make_seeded_capability_profile(DeviceType::Dcmt);
    mismatch.detail = "wrong type";

    const InventoryBuildResult result =
        build_inventory_from_probes(config, {dcmt_baseline, mismatch}, InventorySource::Manual);

    ASSERT_EQ(result.supported_devices.size(), 1U);
    EXPECT_EQ(result.supported_devices[0].descriptor.device_id(), "dcmt0");
    EXPECT_EQ(result.supported_devices[0].capability_profile.source, CapabilitySource::BaselineFallback);
    EXPECT_EQ(result.supported_devices[0].capabilities.functions_size(), 3);
    EXPECT_TRUE(function_exists(result.supported_devices[0], 1, ""));
    EXPECT_TRUE(function_exists(result.supported_devices[0], 2, ""));
    EXPECT_TRUE(function_exists(result.supported_devices[0], 3, ""));
    EXPECT_FALSE(function_exists(result.supported_devices[0], 4, ""));
    EXPECT_FALSE(function_exists(result.supported_devices[0], 5, ""));

    ASSERT_EQ(result.unsupported_probes.size(), 1U);
    EXPECT_EQ(result.unsupported_probes[0].status, ProbeStatus::TypeMismatch);
    ASSERT_EQ(result.missing_expected_ids.size(), 1U);
    EXPECT_EQ(result.missing_expected_ids[0], "heater0");
}

// Inventory for a single queried device of @p type at 0x10 with @p flags.
InventoryDevice queried_device(DeviceType type, uint32_t flags) {
    ProbeRecord probe;
    probe.address = 0x10;
    probe.type_id = bread_type_id(type);
    probe.status = ProbeStatus::Supported;
    probe.version = {CRUMBS_VERSION, 1, 0, 0};
    probe.capability_profile.schema = BREAD_CAPS_SCHEMA_V1;
    probe.capability_profile.level = 1;
    probe.capability_profile.flags = flags;
    probe.capability_profile.source = CapabilitySource::Queried;
    const InventoryBuildResult result =
        build_inventory_from_probes(make_base_config(), {probe}, InventorySource::Discovered);
    EXPECT_EQ(result.supported_devices.size(), 1U);
    return result.supported_devices.at(0);
}

const anolis::deviceprovider::v1::FunctionSpec *find_function(const InventoryDevice &device, const std::string &name) {
    for (const auto &fn : device.capabilities.functions()) {
        if (fn.name() == name) {
            return &fn;
        }
    }
    return nullptr;
}

TEST(BreadInventoryTest, ClearWatchdogTripOfferedOnlyWithBothWatchdogCaps) {
    struct Case {
        DeviceType type;
        uint32_t base;
        uint32_t cmd_watchdog;
        uint32_t clear_trip;
        uint32_t function_id;
    };
    const Case cases[] = {
        {DeviceType::Rlht, RLHT_CAP_BASELINE_FLAGS, RLHT_CAP_CMD_WATCHDOG, RLHT_CAP_CLEAR_WATCHDOG_TRIP, 7},
        {DeviceType::Dcmt, DCMT_CAP_BASELINE_FLAGS, DCMT_CAP_CMD_WATCHDOG, DCMT_CAP_CLEAR_WATCHDOG_TRIP, 6},
    };
    for (const Case &c : cases) {
        SCOPED_TRACE(to_string(c.type));

        const InventoryDevice both = queried_device(c.type, c.base | c.cmd_watchdog | c.clear_trip);
        const auto *fn = find_function(both, "clear_watchdog_trip");
        ASSERT_NE(fn, nullptr);
        EXPECT_EQ(fn->function_id(), c.function_id);
        EXPECT_EQ(fn->policy().category(), anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_CONFIG);
        EXPECT_EQ(fn->args_size(), 0);

        // Old firmware: watchdog without the latch. SET_WATCHDOG still clears
        // the trip there, so the clear op must not be offered.
        EXPECT_EQ(find_function(queried_device(c.type, c.base | c.cmd_watchdog), "clear_watchdog_trip"), nullptr);
        EXPECT_FALSE(function_exists(queried_device(c.type, c.base | c.cmd_watchdog), c.function_id, ""));
        // The clear bit alone is not enough either.
        EXPECT_EQ(find_function(queried_device(c.type, c.base | c.clear_trip), "clear_watchdog_trip"), nullptr);
    }
}

TEST(BreadInventoryTest, DcmtSpeedPidOfferedOnlyWithClosedLoopSpeedCap) {
    const uint32_t base = DCMT_CAP_BASELINE_FLAGS | DCMT_CAP_CLOSED_LOOP_POSITION | DCMT_CAP_PID_TUNING;

    const InventoryDevice position_only = queried_device(DeviceType::Dcmt, base);
    const auto *position_pid = find_function(position_only, "set_pid_x1000");
    ASSERT_NE(position_pid, nullptr);
    EXPECT_EQ(position_pid->function_id(), 5u);
    ASSERT_EQ(position_pid->args_size(), 6);
    for (const auto &arg : position_pid->args()) {
        EXPECT_NE(arg.name().find("_x1000"), std::string::npos) << arg.name();
        EXPECT_EQ(arg.type(), anolis::deviceprovider::v1::VALUE_TYPE_UINT64);
        EXPECT_EQ(arg.max_uint64(), 65535u);
    }
    EXPECT_EQ(find_function(position_only, "set_speed_pid_x1000"), nullptr);
    EXPECT_FALSE(function_exists(position_only, 7, ""));

    const InventoryDevice with_speed = queried_device(DeviceType::Dcmt, base | DCMT_CAP_CLOSED_LOOP_SPEED);
    const auto *speed_pid = find_function(with_speed, "set_speed_pid_x1000");
    ASSERT_NE(speed_pid, nullptr);
    EXPECT_EQ(speed_pid->function_id(), 7u);
    EXPECT_EQ(speed_pid->args_size(), 6);
    EXPECT_EQ(speed_pid->policy().category(), anolis::deviceprovider::v1::FunctionPolicy_Category_CATEGORY_CONFIG);

    // Speed without PID tuning offers neither.
    const InventoryDevice no_tuning =
        queried_device(DeviceType::Dcmt, DCMT_CAP_BASELINE_FLAGS | DCMT_CAP_CLOSED_LOOP_SPEED);
    EXPECT_EQ(find_function(no_tuning, "set_pid_x1000"), nullptr);
    EXPECT_EQ(find_function(no_tuning, "set_speed_pid_x1000"), nullptr);
}

// A DCMT configured with encoders: false offers no closed-loop control (#179):
// the firmware advertises the loops whether or not encoders are fitted.
TEST(BreadInventoryTest, DcmtWithoutEncodersOffersNoClosedLoopFunctions) {
    const uint32_t full =
        DCMT_CAP_BASELINE_FLAGS | DCMT_CAP_CLOSED_LOOP_POSITION | DCMT_CAP_CLOSED_LOOP_SPEED | DCMT_CAP_PID_TUNING;
    ProviderConfig config = make_base_config();
    config.discovery_mode = DiscoveryMode::Manual;
    config.manual_addresses = {0x14, 0x15};
    DeviceSpec no_encoders{"drive-open", DeviceType::Dcmt, "Open Drive", 0x14};
    no_encoders.encoders = false;
    DeviceSpec fitted{"drive-servo", DeviceType::Dcmt, "Servo Drive", 0x15};
    config.devices = {no_encoders, fitted};

    auto probe_at = [&](int address) {
        ProbeRecord probe;
        probe.address = address;
        probe.type_id = DCMT_TYPE_ID;
        probe.status = ProbeStatus::Supported;
        probe.version = {CRUMBS_VERSION, 1, 0, 0};
        probe.capability_profile.schema = BREAD_CAPS_SCHEMA_V1;
        probe.capability_profile.level = 1;
        probe.capability_profile.flags = full;
        probe.capability_profile.source = CapabilitySource::Queried;
        return probe;
    };
    const InventoryBuildResult result =
        build_inventory_from_probes(config, {probe_at(0x14), probe_at(0x15)}, InventorySource::Manual);
    ASSERT_EQ(result.supported_devices.size(), 2U);

    for (const auto &device : result.supported_devices) {
        const bool open = device.descriptor.device_id() == "drive-open";
        EXPECT_EQ(device.encoders, !open);
        for (const char *name : {"set_setpoint", "set_pid_x1000", "set_speed_pid_x1000"}) {
            EXPECT_EQ(find_function(device, name) == nullptr, open) << device.descriptor.device_id() << " " << name;
        }
        EXPECT_NE(find_function(device, "set_open_loop"), nullptr);
        const auto *set_mode = find_function(device, "set_mode");
        ASSERT_NE(set_mode, nullptr);
        ASSERT_EQ(set_mode->args_size(), 1);
        const std::string &modes = set_mode->args(0).description();
        EXPECT_EQ(modes.find("closed_position") == std::string::npos, open) << modes;
        // The profile and its tag still report what the firmware advertised.
        EXPECT_EQ(device.capability_profile.flags & DCMT_CAP_CLOSED_LOOP_POSITION, DCMT_CAP_CLOSED_LOOP_POSITION);
    }
    EXPECT_EQ(result.supported_devices[0].descriptor.tags().at("caps_flags"),
              result.supported_devices[1].descriptor.tags().at("caps_flags"));
}

TEST(BreadInventoryTest, EstopCountSignalFollowsTheContract) {
    // DCMT module major 2 always carries estop_count; RLHT only with its caps bit.
    EXPECT_TRUE(signal_exists(queried_device(DeviceType::Dcmt, DCMT_CAP_BASELINE_FLAGS), "estop_count"));
    EXPECT_FALSE(signal_exists(queried_device(DeviceType::Rlht, RLHT_CAP_BASELINE_FLAGS), "estop_count"));
    EXPECT_TRUE(
        signal_exists(queried_device(DeviceType::Rlht, RLHT_CAP_BASELINE_FLAGS | RLHT_CAP_ESTOP_COUNT), "estop_count"));
}

}  // namespace
}  // namespace anolis_provider_bread::inventory
