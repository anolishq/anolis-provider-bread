#pragma once

/**
 * @file startup.hpp
 * @brief Discovery helpers that probe BREAD devices and assemble runtime
 * inventory.
 */

#include <chrono>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "config/provider_config.hpp"
#include "crumbs/session.hpp"
#include "devices/common/inventory.hpp"

namespace anolis_provider_bread::startup {

/**
 * @brief Inventory result returned from one startup discovery pass.
 */
struct DiscoveryResult {
    std::vector<inventory::InventoryDevice> devices;
    std::vector<inventory::ProbeRecord> unsupported_probes;
    std::vector<std::string> missing_expected_ids;
    // Probe-failure detail per missing-expected id (#104); see
    // InventoryBuildResult::missing_expected_details.
    std::map<std::string, std::string> missing_expected_details;
    std::string inventory_mode;
};

/**
 * @brief Probe one I2C address through the BREAD discovery sequence.
 *
 * Error handling:
 * Returns a `ProbeRecord` with a non-supported status on probe failure or
 * incompatibility. This function does not throw for per-address I/O failures.
 */
inventory::ProbeRecord probe_device(crumbs::Session &session, uint8_t address);

/**
 * @brief Run the configured discovery flow and build the supported inventory.
 *
 * With a `deadline`, no address is probed once it has passed. An expected
 * device whose address was left unprobed is reported missing as "not probed"
 * (anolishq/anolis-protocol#76), so start-up fits the runtime's WaitReady
 * budget on a bus that is down. A probe that starts before the deadline runs to
 * the end.
 *
 * Error handling:
 * Throws `std::runtime_error` only when the bus-wide scan operation fails in
 * scan mode. Unsupported or missing devices are reported in the result.
 */
DiscoveryResult run_discovery(crumbs::Session &session, const ProviderConfig &config,
                              std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);

}  // namespace anolis_provider_bread::startup
