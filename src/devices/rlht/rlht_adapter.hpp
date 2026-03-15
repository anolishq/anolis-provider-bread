#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "crumbs/session.hpp"
#include "devices/common/adapter_helpers.hpp"
#include "devices/common/inventory.hpp"

namespace anolis_provider_bread::rlht {

// Read RLHT signals from the device.
//
// Performs a single RLHT_OP_GET_STATE query regardless of which signal_ids
// are requested. If signal_ids is empty all signals are returned; otherwise
// only the requested subset is returned. The caller is responsible for
// ensuring all requested signal_ids are valid (use signal_exists before
// calling this).
AdapterReadResult read_signals(crumbs::Session &session,
                               const inventory::InventoryDevice &device,
                               const std::vector<std::string> &signal_ids);

// Execute an RLHT function call.
//
// function_id must be non-zero. The caller is responsible for resolving
// function_name to function_id before calling (look up by name in
// device.capabilities if needed).
AdapterCallResult call(crumbs::Session &session,
                       const inventory::InventoryDevice &device,
                       uint32_t function_id,
                       const ValueMap &args);

} // namespace anolis_provider_bread::rlht
