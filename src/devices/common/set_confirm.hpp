#pragma once

/**
 * @file set_confirm.hpp
 * @brief Send a SET and confirm it from the device's CRUMBS acknowledgement
 * (GET_ACK).
 *
 * The provider's half of anolis-provider-bread#134: a setter call reports what
 * the device did with the frame, not that the frame was written.
 */

#include <cstdint>
#include <string>

#include "crumbs/session.hpp"
#include "devices/common/adapter_helpers.hpp"
#include "devices/common/inventory.hpp"

namespace anolis_provider_bread {

/** @brief Sends of one SET in all while the device reports it not advanced
 * (CRUMBS' CONFIRMED_SENDS). */
inline constexpr int kConfirmedSends = 3;

/**
 * @brief How a device's SETs are confirmed, from its probe.
 *
 * - `Acknowledged`: CRUMBS 1600 or later and the family's `*_CAP_REFUSES` bit.
 *   GET_ACK says applied, refused (with the firmware's reason) or rejected.
 * - `DeliveryOnly`: CRUMBS 1600 or later without `*_CAP_REFUSES`. GET_ACK
 *   APPLIED means the frame was dispatched; the firmware does not refuse.
 * - `Unconfirmed`: CRUMBS older than 1600 has no GET_ACK. The call is OK once
 *   the frame is written, as before.
 */
enum class SetConfirmation {
    Acknowledged,
    DeliveryOnly,
    Unconfirmed,
};

/** @brief Classify a device by its probed CRUMBS version and capability flags. */
SetConfirmation set_confirmation(const inventory::InventoryDevice &device);

/** @brief Stable string for the `set_confirmation` health metric. */
std::string to_string(SetConfirmation confirmation);

/**
 * @brief Send @p frame to @p device and report the outcome per call.
 *
 * For a device at CRUMBS 1600 or later, mirrors CRUMBS'
 * crumbs_controller_send_confirmed() over the session: read GET_ACK (the
 * baseline), send, read GET_ACK again and classify with crumbs_ack_classify().
 * The probe's version is used; no version query is made per call.
 *
 * - APPLIED: OK.
 * - REFUSED: an error naming the reason ("refused: ESTOP").
 * - REJECTED: CODE_INTERNAL naming the CRUMBS reject reason.
 * - Not advanced (the record equals the baseline): sent again, up to
 *   kConfirmedSends sends in all, then CODE_UNAVAILABLE.
 * - Another command counted, or a record the counter cannot account for: an
 *   error, not re-sent.
 * - A baseline that cannot be read: CODE_UNAVAILABLE with nothing sent.
 * - A GET_ACK that cannot be read after a send: CODE_UNAVAILABLE, not re-sent,
 *   since the frame may have been applied.
 *
 * A failed write is still followed by the GET_ACK read, which decides, as in
 * CRUMBS. A device below CRUMBS 1600 gets the frame written once and an OK.
 *
 * @param op Names the call in error messages, e.g. "RLHT SET command".
 */
AdapterCallResult transmit_confirmed(crumbs::Session &session, const inventory::InventoryDevice &device,
                                     const crumbs::RawFrame &frame, const std::string &op);

}  // namespace anolis_provider_bread
