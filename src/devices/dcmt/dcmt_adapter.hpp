#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "crumbs/session.hpp"
#include "devices/common/adapter_helpers.hpp"
#include "devices/common/inventory.hpp"

namespace anolis_provider_bread::dcmt {

// Read DCMT signals from the device.
//
// Performs a single DCMT_OP_GET_STATE query. Handles both open-loop (7-byte)
// and closed-loop (11-byte) frame layouts transparently. If signal_ids is
// empty all signals are returned; otherwise only the requested subset is
// returned. The caller is responsible for ensuring all requested signal_ids
// are valid (use signal_exists before calling this).
AdapterReadResult read_signals(crumbs::Session &session,
                               const inventory::InventoryDevice &device,
                               const std::vector<std::string> &signal_ids);

// Execute a DCMT function call.
//
// function_id must be non-zero. The caller is responsible for resolving
// function_name to function_id before calling.
AdapterCallResult call(crumbs::Session &session,
                       const inventory::InventoryDevice &device,
                       uint32_t function_id,
                       const ValueMap &args);

} // namespace anolis_provider_bread::dcmt
