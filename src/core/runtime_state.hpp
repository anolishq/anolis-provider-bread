#pragma once

/**
 * @file runtime_state.hpp
 * @brief Process-wide runtime snapshot and session access for the BREAD provider.
 */

#include <chrono>
#include <string>
#include <vector>

#include "config/provider_config.hpp"
#include "devices/common/inventory.hpp"

// Forward declaration — callers that need the full Session type must include
// crumbs/session.hpp themselves.
namespace anolis_provider_bread::crumbs {
class Session;
} // namespace anolis_provider_bread::crumbs

namespace anolis_provider_bread::runtime {

/**
 * @brief Snapshot of provider startup state exposed to handlers and health code.
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
    // Discovery diagnostics
    std::string inventory_mode;
    int unsupported_probe_count = 0;
    std::vector<std::string> missing_expected_ids;
};

/** @brief Reset runtime state and destroy any live session/transport pair. */
void reset();

/** @brief Initialize runtime state from config, including live discovery when available. */
void initialize(const ProviderConfig &config);

/** @brief Return a copy of the current runtime snapshot. */
RuntimeState snapshot();

/**
 * @brief Return the live CRUMBS session, or `nullptr` when no live session exists.
 *
 * Lifetime:
 * The returned pointer remains valid until the next `reset()` or successful
 * `initialize()` call replaces the owned session.
 */
crumbs::Session *session();

} // namespace anolis_provider_bread::runtime
