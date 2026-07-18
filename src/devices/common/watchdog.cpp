#include "devices/common/watchdog.hpp"

#include <string>

#include "logging/logger.hpp"

extern "C" {
#include <bread/bread_watchdog.h>
#include <bread/dcmt_ops.h>
#include <bread/rlht_ops.h>
}

namespace anolis_provider_bread::watchdog {
namespace {

uint32_t capability_flag_for(DeviceType type) {
    switch (type) {
        case DeviceType::Rlht:
            return RLHT_CAP_CMD_WATCHDOG;
        case DeviceType::Dcmt:
            return DCMT_CAP_CMD_WATCHDOG;
    }
    return 0;
}

}  // namespace

bool capability_supported(const inventory::InventoryDevice &device) {
    const uint32_t flag = capability_flag_for(device.type);
    return flag != 0 && (device.capability_profile.flags & flag) != 0;
}

void arm_if_configured(crumbs::Session &session, const inventory::InventoryDevice &device, const char *context) {
    if (device.command_watchdog_ms <= 0) {
        return;
    }

    const std::string device_tag = device.descriptor.device_id() + " (" + format_i2c_address(device.address) + ")";

    if (!capability_supported(device)) {
        logging::warning("watchdog: " + device_tag + " has command_watchdog_ms configured but firmware does not " +
                         "advertise CMD_WATCHDOG capability, not arming (" + context + ")");
        return;
    }

    const uint16_t timeout_ms = static_cast<uint16_t>(device.command_watchdog_ms);
    crumbs::RawFrame frame;
    frame.type_id = inventory::bread_type_id(device.type);
    frame.opcode = BREAD_OP_SET_WATCHDOG;
    frame.payload = {static_cast<uint8_t>(timeout_ms & 0xFFu), static_cast<uint8_t>((timeout_ms >> 8) & 0xFFu)};

    const crumbs::SessionStatus status = session.send(static_cast<uint8_t>(device.address), frame);
    if (status) {
        logging::info("watchdog: armed " + device_tag + " at " + std::to_string(timeout_ms) + " ms (" + context + ")");
    } else {
        logging::warning("watchdog: failed to arm " + device_tag + " (" + context + "): " + status.message);
    }
}

crumbs::SessionStatus query_status(crumbs::Session &session, const inventory::InventoryDevice &device,
                                   WatchdogStatus &out) {
    crumbs::RawFrame frame;
    crumbs::SessionStatus status =
        session.query_read(static_cast<uint8_t>(device.address), BREAD_OP_GET_WATCHDOG, frame);
    if (!status) {
        return status;
    }
    if (frame.opcode != BREAD_OP_GET_WATCHDOG) {
        return crumbs::SessionStatus::failure(crumbs::SessionErrorCode::DecodeFailed,
                                              "unexpected watchdog reply opcode");
    }

    bread_watchdog_result_t parsed{};
    if (bread_watchdog_parse_payload(frame.payload.data(), static_cast<uint8_t>(frame.payload.size()), &parsed) != 0) {
        return crumbs::SessionStatus::failure(crumbs::SessionErrorCode::DecodeFailed,
                                              "failed to parse watchdog payload");
    }

    out.armed = parsed.armed != 0;
    out.timeout_ms = parsed.timeout_ms;
    out.tripped = parsed.tripped != 0;
    out.trip_count = parsed.trip_count;
    return crumbs::SessionStatus::success();
}

}  // namespace anolis_provider_bread::watchdog
