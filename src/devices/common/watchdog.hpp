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
 * address-recovery signal (#112), and when a health read finds the device
 * disarmed and not tripped (#148). A local WDOG=0 on the slice serial console
 * is undone the same way; `command_watchdog_ms: 0` keeps a device disarmed.
 *
 * Arming never clears a trip on its own behalf. On firmware advertising
 * *_CAP_CLEAR_WATCHDOG_TRIP the trip latches and SET_WATCHDOG re-arms without
 * releasing it; the only provider path that sends BREAD_OP_CLEAR_WATCHDOG_TRIP
 * is the operator-invoked `clear_watchdog_trip` device function.
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

/**
 * @brief Re-arm a device whose health read shows it disarmed.
 *
 * Firmware boots disarmed, so a device configured to arm that reports
 * `armed == false` has rebooted or was disarmed locally with WDOG=0 (#151);
 * a reboot between two polls fails no operation, so the session's recovery
 * signal never fires (#148). Logs which case it is (a nonzero `trip_count`
 * rules out a reboot, which resets it) and re-arms through
 * arm_if_configured(). A tripped device is left alone: on firmware without the
 * CLEAR_WATCHDOG_TRIP capability SET_WATCHDOG clears a trip, and only an
 * operator may release that hold. No-op when the watchdog is not configured or
 * not supported.
 *
 * @return true when a re-arm was attempted.
 */
bool rearm_if_rebooted(crumbs::Session &session, const inventory::InventoryDevice &device,
                       const WatchdogStatus &status);

/** @brief Query the device's watchdog status over the bus. */
crumbs::SessionStatus query_status(crumbs::Session &session, const inventory::InventoryDevice &device,
                                   WatchdogStatus &out);

}  // namespace anolis_provider_bread::watchdog
