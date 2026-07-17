#pragma once

/**
 * @file watchdog.hpp
 * @brief Firmware command-watchdog helpers (BREAD_OP_SET/GET_WATCHDOG).
 *
 * The watchdog is armed per device from `devices[].command_watchdog_ms` and
 * only when the probed capability flags advertise support
 * (DCMT_CAP_CMD_WATCHDOG / RLHT_CAP_CMD_WATCHDOG), so old firmware never sees
 * the opcode. Firmware boots disarmed, and a device reboot (e.g. an e-stop
 * that cuts backplane power) resets it — callers re-arm on the session's
 * address-recovery signal (#112).
 */

#include <cstdint>

#include "crumbs/session.hpp"
#include "devices/common/inventory.hpp"

namespace anolis_provider_bread::watchdog {

/**
 * @brief Firmware-reported watchdog status (BREAD_OP_GET_WATCHDOG payload).
 */
struct WatchdogStatus {
    bool armed = false;
    uint16_t timeout_ms = 0;
    bool tripped = false;
    uint8_t trip_count = 0;
};

/** @brief Report whether the device's capability flags advertise the watchdog. */
bool capability_supported(const inventory::InventoryDevice &device);

/**
 * @brief Arm the device's watchdog when configured and supported; log outcome.
 *
 * No-op when `command_watchdog_ms` is 0. When configured but the capability
 * flag is absent, logs a warning once per call and sends nothing. A failed
 * send is logged but not fatal: the next recovery re-arm attempt retries.
 */
void arm_if_configured(crumbs::Session &session, const inventory::InventoryDevice &device, const char *context);

/** @brief Query the device's watchdog status over the bus. */
crumbs::SessionStatus query_status(crumbs::Session &session, const inventory::InventoryDevice &device,
                                   WatchdogStatus &out);

}  // namespace anolis_provider_bread::watchdog
