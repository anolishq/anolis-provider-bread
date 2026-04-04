#pragma once

/**
 * @file inventory.hpp
 * @brief Inventory and capability-building helpers for discovered or seeded BREAD devices.
 */

#include <string>
#include <vector>

#include "config/provider_config.hpp"
#include "devices/common/bread_compatibility.hpp"
#include "protocol.pb.h"

namespace anolis_provider_bread::inventory {

using Device = anolis::deviceprovider::v1::Device;
using CapabilitySet = anolis::deviceprovider::v1::CapabilitySet;

/**
 * @brief How a device entered the runtime inventory.
 */
enum class InventorySource {
    ConfigSeeded,
    Manual,
    Discovered,
};

/**
 * @brief Origin of the capability flags used to expose a device contract.
 */
enum class CapabilitySource {
    Seeded,
    Queried,
    BaselineFallback,
};

/**
 * @brief Normalized capability metadata for one inventory device.
 */
struct CapabilityProfile {
    uint8_t schema = 0;
    uint8_t level = 0;
    uint32_t flags = 0;
    CapabilitySource source = CapabilitySource::Seeded;
};

/**
 * @brief Raw per-address discovery result captured during probing.
 */
struct ProbeRecord {
    int address = 0;
    uint8_t type_id = 0;
    ProbeStatus status = ProbeStatus::UnsupportedType;
    ModuleVersion version;
    CapabilityProfile capability_profile;
    std::string detail;
};

/**
 * @brief Supported device entry published into the runtime inventory.
 */
struct InventoryDevice {
    Device descriptor;
    CapabilitySet capabilities;
    DeviceType type = DeviceType::Rlht;
    int address = 0;
    InventorySource source = InventorySource::ConfigSeeded;
    bool expected = false;
    ModuleVersion version;
    CapabilityProfile capability_profile;
};

/**
 * @brief Inventory assembly output including excluded or missing devices.
 */
struct InventoryBuildResult {
    std::vector<InventoryDevice> supported_devices;
    std::vector<ProbeRecord> unsupported_probes;
    std::vector<std::string> missing_expected_ids;
};

/** @brief Build synthetic probe records for config-seeded no-hardware inventory. */
std::vector<ProbeRecord> build_seed_probes(const ProviderConfig &config);

/**
 * @brief Build supported inventory from probe records and expected config entries.
 */
InventoryBuildResult build_inventory_from_probes(const ProviderConfig &config,
                                                 const std::vector<ProbeRecord> &probes,
                                                 InventorySource source);

/** @brief Build config-seeded inventory without touching live hardware. */
std::vector<InventoryDevice> build_seed_inventory(const ProviderConfig &config);

/** @brief Return the baseline fallback capability profile for a device type. */
CapabilityProfile make_baseline_capability_profile(DeviceType type);

/** @brief Return the config-seeded capability profile for a device type. */
CapabilityProfile make_seeded_capability_profile(DeviceType type);

/** @brief Convert an inventory source enum to its stable tag/debug string form. */
std::string to_string(InventorySource source);

/** @brief Convert a capability source enum to its stable tag/debug string form. */
std::string to_string(CapabilitySource source);

/** @brief Find one device in the inventory by provider-local device ID. */
const InventoryDevice *find_device(const std::vector<InventoryDevice> &devices, const std::string &device_id);

/** @brief Report whether a device exposes the named signal in its capabilities. */
bool signal_exists(const InventoryDevice &device, const std::string &signal_id);

/**
 * @brief Report whether a device exposes the requested function selector.
 */
bool function_exists(const InventoryDevice &device, uint32_t function_id, const std::string &function_name);

} // namespace anolis_provider_bread::inventory
