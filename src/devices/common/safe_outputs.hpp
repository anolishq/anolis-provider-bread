#pragma once

/**
 * @file safe_outputs.hpp
 * @brief Drive a device's outputs to safe values (anolishq/anolis-provider-bread#195).
 *
 * The provider runs this for each device after discovery, before it reports
 * ready, and again when a device comes back after I/O failures (a reboot, a
 * power cut, a re-address), unless `provider.outputs_on_startup` is `leave`.
 * The values are the sequence the bioreactor's `safety.safe_state` hooks send:
 *
 * - DCMT: set_mode open_loop, then set_open_loop 0/0. The brakes are left as
 *   they are.
 * - RLHT: set_setpoints 0/0, set_mode open_loop, then set_open_duty_pct 0/0.
 *   Each is safe if another is lost: a zero setpoint turns a closed loop off
 *   on current firmware, and open loop at zero duty leaves both relays open.
 *
 * Each call goes through the device's adapter, so it is confirmed from
 * GET_ACK like any setter call (set_confirm.hpp). Every step is sent even if
 * an earlier one fails.
 */

#include <string>

#include "crumbs/session.hpp"
#include "devices/common/inventory.hpp"

namespace anolis_provider_bread::safe_outputs {

/** @brief Outcome of driving one device's outputs safe. */
struct Result {
    bool ok = true;
    /** @brief Each step that failed and why, e.g. "set_open_loop: refused: ESTOP". */
    std::string failures;
};

/** @brief Send @p device's safe-output sequence over @p session. */
Result drive(crumbs::Session &session, const inventory::InventoryDevice &device);

/** @brief The `safe_outputs` health metric for a result: "set" or "not set: ...". */
std::string to_metric(const Result &result);

}  // namespace anolis_provider_bread::safe_outputs
