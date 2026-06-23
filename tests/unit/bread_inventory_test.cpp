#include <gtest/gtest.h>

#include "devices/common/inventory.hpp"

extern "C" {
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
    EXPECT_EQ(result.supported_devices[0].descriptor.tags().at("hw.bus_path"), "/dev/i2c-1");
    EXPECT_EQ(result.supported_devices[0].descriptor.tags().at("hw.i2c_address"), "0x08");
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

}  // namespace
}  // namespace anolis_provider_bread::inventory
