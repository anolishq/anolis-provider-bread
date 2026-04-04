#pragma once

/**
 * @file provider_config.hpp
 * @brief Manual configuration types and parsing entry points for anolis-provider-bread.
 */

#include <string>
#include <vector>

namespace anolis_provider_bread {

/**
 * @brief Inventory acquisition mode for provider startup.
 */
enum class DiscoveryMode {
    Scan,
    Manual,
};

/**
 * @brief Supported BREAD device families exposed by this provider.
 */
enum class DeviceType {
    Rlht,
    Dcmt,
};

/**
 * @brief One expected device entry from the provider config.
 *
 * `address` and `type` together define the expected hardware identity used to
 * name and validate devices during discovery.
 */
struct DeviceSpec {
    std::string id;
    DeviceType type = DeviceType::Rlht;
    std::string label;
    int address = 0;
};

/**
 * @brief Fully resolved provider configuration after YAML parsing.
 *
 * The provider may either scan the bus or probe the explicitly listed manual
 * addresses. `require_live_session` forces startup to fail on no-hardware
 * builds instead of falling back to config-seeded inventory.
 */
struct ProviderConfig {
    std::string config_file_path;
    std::string provider_name = "anolis-provider-bread";
    std::string bus_path;
    bool require_live_session = false;
    int query_delay_us = 10000;
    int timeout_ms = 100;
    int retry_count = 2;
    DiscoveryMode discovery_mode = DiscoveryMode::Scan;
    std::vector<int> manual_addresses;
    std::vector<DeviceSpec> devices;
};

/** @brief Load and validate provider configuration from disk. */
ProviderConfig load_config(const std::string &path);

/** @brief Parse a config string into a discovery mode. */
DiscoveryMode parse_discovery_mode(const std::string &value);

/** @brief Parse a config string into a supported BREAD device type. */
DeviceType parse_device_type(const std::string &value);

/** @brief Convert a discovery mode enum to its config/debug string form. */
std::string to_string(DiscoveryMode mode);

/** @brief Convert a device type enum to its config/debug string form. */
std::string to_string(DeviceType type);

/** @brief Format a 7-bit I2C address as canonical `0xNN` text. */
std::string format_i2c_address(int address);

/** @brief Build a short human-readable summary of a resolved config. */
std::string summarize_config(const ProviderConfig &config);

} // namespace anolis_provider_bread
