#pragma once

/**
 * @file provider_config.hpp
 * @brief Manual configuration types and parsing entry points for
 * anolis-provider-bread.
 */

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "devices/common/device_type.hpp"

namespace anolis_provider_bread {

/**
 * @brief Inventory acquisition mode for provider startup.
 */
enum class DiscoveryMode {
    Scan,
    Manual,
};

/**
 * @brief What the provider does to device outputs when it starts, and when a
 * device comes back after I/O failures (#195).
 *
 * `Safe` drives them to safe values (safe_outputs.hpp). `Leave` leaves them as
 * they are, for a machine where a restart must not interrupt a running
 * process, alongside the runtime's `safety.on_startup: leave`.
 */
enum class OutputsOnStartup {
    Safe,
    Leave,
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
    // Firmware command-watchdog timeout (BREAD_OP_SET_WATCHDOG); 0 = never arm.
    // Armed only when the probed capability flags advertise support.
    int command_watchdog_ms = 0;
    // DCMT only: false when no encoders are fitted. The firmware advertises
    // its closed loops either way, so the inventory leaves them out (#179).
    bool encoders = true;
};

/**
 * @brief Fully resolved provider configuration after YAML parsing.
 *
 * The provider may either scan the bus or probe the explicitly listed manual
 * addresses.
 */
struct ProviderConfig {
    std::string config_file_path;
    std::string provider_name = "anolis-provider-bread";
    OutputsOnStartup outputs_on_startup = OutputsOnStartup::Safe;
    std::string bus_path;
    int query_delay_us = 10000;
    int timeout_ms = 100;
    int retry_count = 2;
    // Highest bus clock the devices tolerate on this machine; unset skips the
    // i2c.bus_clock host check.
    std::optional<uint32_t> max_bus_hz;
    DiscoveryMode discovery_mode = DiscoveryMode::Manual;
    std::vector<int> manual_addresses;
    std::vector<DeviceSpec> devices;
};

/** @brief Load and validate provider configuration from disk. */
ProviderConfig load_config(const std::string &path);

/** @brief Parse a config string into a discovery mode. */
DiscoveryMode parse_discovery_mode(const std::string &value);

/** @brief Parse a config string into an outputs-on-startup policy. */
OutputsOnStartup parse_outputs_on_startup(const std::string &value);

/** @brief Parse a config string into a supported BREAD device type. */
DeviceType parse_device_type(const std::string &value);

/** @brief Convert a discovery mode enum to its config/debug string form. */
std::string to_string(DiscoveryMode mode);

/** @brief Convert an outputs-on-startup policy to its config string form. */
std::string to_string(OutputsOnStartup policy);

/** @brief Convert a device type enum to its config/debug string form. */
std::string to_string(DeviceType type);

/** @brief Format a 7-bit I2C address as canonical `0xNN` text. */
std::string format_i2c_address(int address);

/** @brief Build a short human-readable summary of a resolved config. */
std::string summarize_config(const ProviderConfig &config);

}  // namespace anolis_provider_bread
