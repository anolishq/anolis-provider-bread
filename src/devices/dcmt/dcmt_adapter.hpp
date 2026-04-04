#pragma once

/**
 * @file dcmt_adapter.hpp
 * @brief DCMT-specific signal and function adapters on top of the generic CRUMBS session API.
 */

#include <cstdint>
#include <string>
#include <vector>

#include "crumbs/session.hpp"
#include "devices/common/adapter_helpers.hpp"
#include "devices/common/inventory.hpp"

namespace anolis_provider_bread::dcmt {

/**
 * @brief Read one coherent DCMT state snapshot and project it into ADPP signals.
 *
 * Performs a single `DCMT_OP_GET_STATE` query and handles both the open-loop
 * and closed-loop payload layouts transparently. If `signal_ids` is empty all
 * supported signals are returned; otherwise only the requested subset is
 * emitted.
 */
AdapterReadResult read_signals(crumbs::Session &session,
                               const inventory::InventoryDevice &device,
                               const std::vector<std::string> &signal_ids);

/**
 * @brief Encode and send one DCMT control function call.
 *
 * Preconditions:
 * `function_id` must already be resolved from the device capability metadata.
 */
AdapterCallResult call(crumbs::Session &session,
                       const inventory::InventoryDevice &device,
                       uint32_t function_id,
                       const ValueMap &args);

} // namespace anolis_provider_bread::dcmt
