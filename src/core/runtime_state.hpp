#pragma once

/**
 * @file runtime_state.hpp
 * @brief Process-wide runtime snapshot and session access for the BREAD
 * provider.
 */

#include <chrono>
#include <map>
#include <string>
#include <vector>

#include "anolis/provider_sdk/host_check.hpp"
#include "config/provider_config.hpp"
#include "devices/common/inventory.hpp"

// Forward declaration — callers that need the full Session type must include
// crumbs/session.hpp themselves.
namespace anolis_provider_bread::crumbs {
class Session;
}  // namespace anolis_provider_bread::crumbs

namespace anolis_provider_bread::runtime {

/**
 * @brief Snapshot of provider startup state exposed to handlers and health
 * code.
 *
 * The live CRUMBS session is stored separately and accessed through
 * `session()`. This snapshot keeps the inventory and startup diagnostics that
 * handlers need without exposing session ownership details.
 */
struct RuntimeState {
    ProviderConfig config;
    std::vector<inventory::InventoryDevice> devices;
    bool ready = false;
    std::string startup_message;
    std::chrono::system_clock::time_point started_at;
    // Set when the provider first becomes ready; init_time_ms = ready_at - started_at.
    std::chrono::system_clock::time_point ready_at;
    // Discovery diagnostics
    std::string inventory_mode;
    int unsupported_probe_count = 0;
    std::vector<std::string> missing_expected_ids;
    // Probe-failure detail per missing-expected id (#104), when known.
    std::map<std::string, std::string> missing_expected_details;
    // Host requirements checked at startup (executable profile v1 §6); empty in
    // mock mode. When one is unmet, or the bus will not open, the provider stays
    // up and not ready with no devices instead of exiting.
    std::vector<anolis::provider_sdk::host_check::Requirement> host_requirements;
};

/** @brief Reset runtime state and destroy any live session/transport pair. */
void reset();

/**
 * @brief Initialize runtime state from config, including live discovery when
 * available.
 *
 * `budget` is the time the caller can wait, the runtime's WaitReady
 * `max_wait_ms_hint` (anolishq/anolis-protocol#76); 0 means no limit.
 * Discovery stops probing kDiscoveryMargin before it runs out, leaving room for
 * a probe already under way, and expected devices left unprobed are reported
 * missing. A start-up that throws, such as a failed bus scan in scan mode,
 * leaves the provider up and not ready with the reason, the same as an
 * unmet host requirement.
 */
void initialize(const ProviderConfig &config, std::chrono::milliseconds budget = std::chrono::milliseconds{0});

/**
 * @brief How long before the end of the budget discovery stops starting probes.
 *
 * One probe of an address that does not answer waits out the bus: about 1 s
 * per failed write with the backplane unpowered (2026-10-19). The margin
 * covers that probe with room to spare.
 */
inline constexpr std::chrono::milliseconds kDiscoveryMargin{3000};

/** @brief Return a copy of the current runtime snapshot. */
RuntimeState snapshot();

/**
 * @brief Return the live CRUMBS session, or `nullptr` when no live session
 * exists.
 *
 * Lifetime:
 * The returned pointer remains valid until the next `reset()` or successful
 * `initialize()` call replaces the owned session.
 */
crumbs::Session *session();

}  // namespace anolis_provider_bread::runtime
