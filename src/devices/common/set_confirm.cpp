#include "devices/common/set_confirm.hpp"

/**
 * @file set_confirm.cpp
 * @brief GET_ACK confirmation of SET frames (anolis-provider-bread#134).
 */

#include <cstdio>
#include <cstring>
#include <string>

#include "logging/logger.hpp"

extern "C" {
#include <bread/bread_refuse.h>
#include <bread/dcmt_ops.h>
#include <bread/rlht_ops.h>

#include "crumbs.h"
}

namespace anolis_provider_bread {
namespace {

using Status = anolis::deviceprovider::v1::Status;

std::string hex_byte(uint8_t value) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "0x%02X", static_cast<unsigned>(value));
    return buf;
}

uint32_t refuses_flag_for(DeviceType type) {
    switch (type) {
        case DeviceType::Rlht:
            return RLHT_CAP_REFUSES;
        case DeviceType::Dcmt:
            return DCMT_CAP_REFUSES;
    }
    return 0;
}

struct Refusal {
    const char *name;
    Status::Code code;
};

// By cause: the device's state (e-stop, trip, mode) is a precondition the
// caller can clear; a malformed, out-of-range or unknown command is the
// caller's argument; a feature the build left out is unimplemented.
Refusal refusal_for(uint8_t reason) {
    switch (reason) {
        case BREAD_REFUSE_ESTOP:
            return {"ESTOP", Status::CODE_FAILED_PRECONDITION};
        case BREAD_REFUSE_WATCHDOG_TRIP:
            return {"WATCHDOG_TRIP", Status::CODE_FAILED_PRECONDITION};
        case BREAD_REFUSE_WRONG_MODE:
            return {"WRONG_MODE", Status::CODE_FAILED_PRECONDITION};
        case BREAD_REFUSE_MALFORMED:
            return {"MALFORMED", Status::CODE_INVALID_ARGUMENT};
        case BREAD_REFUSE_UNDEFINED_VALUE:
            return {"UNDEFINED_VALUE", Status::CODE_INVALID_ARGUMENT};
        case BREAD_REFUSE_UNKNOWN_OP:
            return {"UNKNOWN_OP", Status::CODE_INVALID_ARGUMENT};
        case BREAD_REFUSE_NOT_BUILT:
            return {"NOT_BUILT", Status::CODE_UNIMPLEMENTED};
        default:
            // 0x80-0xFF belong to the families and none is defined yet.
            return {nullptr, Status::CODE_FAILED_PRECONDITION};
    }
}

std::string reject_name(uint8_t reason) {
    switch (reason) {
        case CRUMBS_ACK_REJECT_NO_HANDLER:
            return "NO_HANDLER";
        case CRUMBS_ACK_REJECT_TYPE:
            return "TYPE";
        default:
            return hex_byte(reason);
    }
}

// Read the device's acknowledgement. A transport failure returns at once:
// the session has already applied its retry policy, which does not retry a
// bus ETIMEDOUT (#166). A reply that is not a GET_ACK record (another
// opcode was staged, or a corrupt record) is staged and read again, up to
// CRUMBS_ACK_READ_ATTEMPTS, as crumbs_controller_read_ack() does.
bool read_ack(crumbs::Session &session, uint8_t address, crumbs_ack_t &out, std::string &why) {
    for (int attempt = 1; attempt <= CRUMBS_ACK_READ_ATTEMPTS; ++attempt) {
        crumbs::RawFrame reply;
        const crumbs::SessionStatus status = session.query_read(address, CRUMBS_CMD_GET_ACK, reply);
        if (!status) {
            why = status.message;
            return false;
        }
        crumbs_message_t message{};
        message.type_id = reply.type_id;
        message.opcode = reply.opcode;
        message.data_len = static_cast<uint8_t>(reply.payload.size());
        std::memcpy(message.data, reply.payload.data(), reply.payload.size());
        if (crumbs_ack_decode(&message, &out) == 0) {
            return true;
        }
        session.record_rejection(address);
        why = "reply is not a GET_ACK record (opcode " + hex_byte(reply.opcode) + ")";
    }
    return false;
}

}  // namespace

SetConfirmation set_confirmation(const inventory::InventoryDevice &device) {
    if (device.version.crumbs_version < CRUMBS_ACK_MIN_VERSION) {
        return SetConfirmation::Unconfirmed;
    }
    const uint32_t refuses = refuses_flag_for(device.type);
    return refuses != 0 && (device.capability_profile.flags & refuses) != 0 ? SetConfirmation::Acknowledged
                                                                            : SetConfirmation::DeliveryOnly;
}

std::string to_string(SetConfirmation confirmation) {
    switch (confirmation) {
        case SetConfirmation::Acknowledged:
            return "acknowledged";
        case SetConfirmation::DeliveryOnly:
            return "delivery_only";
        case SetConfirmation::Unconfirmed:
            return "unconfirmed";
    }
    return "unconfirmed";
}

AdapterCallResult transmit_confirmed(crumbs::Session &session, const inventory::InventoryDevice &device,
                                     const crumbs::RawFrame &frame, const std::string &op) {
    const auto addr = static_cast<uint8_t>(device.address);

    if (set_confirmation(device) == SetConfirmation::Unconfirmed) {
        const crumbs::SessionStatus send_status = session.send(addr, frame);
        if (!send_status.ok()) {
            return call_error_from_session(send_status, op);
        }
        return {true, Status::CODE_OK, "ok (unconfirmed)"};
    }

    std::string why;
    crumbs_ack_t before{};
    if (!read_ack(session, addr, before, why)) {
        return {false, Status::CODE_UNAVAILABLE, op + " not sent: GET_ACK baseline failed: " + why};
    }

    for (int sent = 1; sent <= kConfirmedSends; ++sent) {
        // A write error is not proof the frame was lost, nor is success proof
        // it arrived: the acknowledgement decides either way.
        const crumbs::SessionStatus send_status = session.send(addr, frame);

        crumbs_ack_t after{};
        if (!read_ack(session, addr, after, why)) {
            // The frame may have been applied; re-sending could apply it twice.
            std::string message = op;
            message += " unconfirmed: GET_ACK after send failed: ";
            message += why;
            if (!send_status.ok()) {
                message += " (the write also failed: " + send_status.message + ")";
            }
            return {false, Status::CODE_UNAVAILABLE, message};
        }

        const int outcome = crumbs_ack_classify(&before, &after, frame.opcode);
        switch (outcome) {
            case CRUMBS_SET_APPLIED:
                return {true, Status::CODE_OK, "ok"};
            case CRUMBS_SET_REFUSED: {
                const Refusal refusal = refusal_for(after.reason);
                std::string message = op;
                message += " refused: ";
                message += refusal.name != nullptr ? refusal.name : hex_byte(after.reason);
                return {false, refusal.code, message};
            }
            case CRUMBS_SET_REJECTED:
                return {false, Status::CODE_INTERNAL, op + " rejected: " + reject_name(after.reason)};
            case CRUMBS_SET_NOT_ADVANCED:
                logging::warning(op + " to " + format_i2c_address(device.address) +
                                 " not counted by the device, send " + std::to_string(sent) + " of " +
                                 std::to_string(kConfirmedSends));
                continue;
            case CRUMBS_RX_ACK_MISMATCH:
                return {
                    false, Status::CODE_UNAVAILABLE,
                    op + " unconfirmed: the device counted another command (opcode " + hex_byte(after.opcode) + ")"};
            case CRUMBS_RX_ACK_UNKNOWN:
                return {false, Status::CODE_UNAVAILABLE,
                        op + " unconfirmed: the acknowledgement cannot say (the device may have restarted)"};
            default:
                return {false, Status::CODE_INTERNAL, op + " unconfirmed: invalid acknowledgement"};
        }
    }

    return {false, Status::CODE_UNAVAILABLE,
            op + " not applied: the device did not count it after " + std::to_string(kConfirmedSends) + " sends"};
}

}  // namespace anolis_provider_bread
