#pragma once

/**
 * @file rlht_adapter.hpp
 * @brief RLHT-specific signal and function adapters on top of the generic CRUMBS session API.
 */

#include <cstdint>
#include <string>
#include <vector>

#include "crumbs/session.hpp"
#include "devices/common/adapter_helpers.hpp"
#include "devices/common/inventory.hpp"

namespace anolis_provider_bread::rlht {

/**
 * @brief Read one coherent RLHT state snapshot and project it into ADPP signals.
 *
 * Performs a single `RLHT_OP_GET_STATE` query regardless of which
 * `signal_ids` are requested. If `signal_ids` is empty all supported signals
 * are returned; otherwise only the requested subset is emitted.
 *
 * Preconditions:
 * Callers are expected to validate requested signal identifiers against the
 * device capabilities before invoking this helper.
 */
AdapterReadResult read_signals(crumbs::Session &session,
                               const inventory::InventoryDevice &device,
                               const std::vector<std::string> &signal_ids);

/**
 * @brief Encode and send one RLHT control function call.
 *
 * Preconditions:
 * `function_id` must already be resolved from the device capability metadata.
 * This helper does not perform name-based selector resolution.
 */
AdapterCallResult call(crumbs::Session &session,
                       const inventory::InventoryDevice &device,
                       uint32_t function_id,
                       const ValueMap &args);

} // namespace anolis_provider_bread::rlht
