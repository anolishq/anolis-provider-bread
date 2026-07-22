#include "crumbs/crumbs_canned_bus.hpp"

/**
 * @file crumbs_canned_bus.cpp
 * @brief Implementation of the canned CRUMBS device bus.
 */

#include <array>
#include <cstring>
#include <utility>
#include <vector>

#include "crumbs/session.hpp"  // kSetReplyOpcode

extern "C" {
#include <bread/bread_caps.h>
#include <bread/bread_watchdog.h>
#include <bread/dcmt_ops.h>
#include <bread/rlht_ops.h>

#include "crumbs.h"
}

namespace anolis_provider_bread::crumbs {
namespace {

namespace sdk_i2c = anolis::provider_sdk::i2c;
using sdk_i2c::I2cError;
using sdk_i2c::I2cStatus;
using sdk_i2c::IoStats;

void append_u8(std::vector<uint8_t> &out, uint8_t value) { out.push_back(value); }

void append_u16_le(std::vector<uint8_t> &out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
}

void append_i16_le(std::vector<uint8_t> &out, int16_t value) { append_u16_le(out, static_cast<uint16_t>(value)); }

// RLHT GET_STATE: open-loop, relay 1 energized, two warm channels. 19 bytes,
// matching rlht_adapter.cpp parse_state (moved verbatim from the former
// MockTransport so mock values are unchanged).
std::vector<uint8_t> build_rlht_state_payload() {
    std::vector<uint8_t> p;
    append_u8(p, RLHT_MODE_OPEN_LOOP);
    append_u8(p, RLHT_FLAG_RELAY1_ON);
    append_i16_le(p, 250);
    append_i16_le(p, 255);
    append_i16_le(p, 300);
    append_i16_le(p, 300);
    append_u16_le(p, 0);
    append_u16_le(p, 0);
    append_u16_le(p, 1000);
    append_u16_le(p, 1000);
    append_u8(p, 0);
    return p;
}

// DCMT GET_STATE: open-loop, motors driven, brakes released. 19 bytes,
// DCMT_STATE_FIXED_LEN, matching dcmt_adapter.cpp parse_state.
std::vector<uint8_t> build_dcmt_state_payload() {
    std::vector<uint8_t> p;
    append_u8(p, DCMT_MODE_OPEN_LOOP);
    append_i16_le(p, 100);
    append_i16_le(p, -100);
    append_i16_le(p, BREAD_INVALID_I16);
    append_i16_le(p, BREAD_INVALID_I16);
    append_i16_le(p, 0);
    append_i16_le(p, 0);
    append_i16_le(p, BREAD_INVALID_I16);
    append_i16_le(p, BREAD_INVALID_I16);
    append_u8(p, 0);
    append_u8(p, 0);
    return p;
}

std::vector<uint8_t> build_watchdog_payload(uint16_t timeout_ms) {
    std::vector<uint8_t> p;
    append_u8(p, timeout_ms != 0 ? 1 : 0);  // armed
    append_u16_le(p, timeout_ms);
    append_u8(p, 0);  // tripped
    append_u8(p, 0);  // trip_count
    return p;
}

}  // namespace

CrumbsCannedBus::CrumbsCannedBus(std::string bus_path) : bus_path_(std::move(bus_path)) {}

void CrumbsCannedBus::add_device(uint8_t address, uint8_t type_id) { devices_[address].type_id = type_id; }

I2cStatus CrumbsCannedBus::open() {
    opened_ = true;
    return I2cStatus::ok();
}

void CrumbsCannedBus::close() { opened_ = false; }
bool CrumbsCannedBus::is_open() const { return opened_; }
const std::string &CrumbsCannedBus::bus_path() const { return bus_path_; }
void CrumbsCannedBus::delay_us(uint32_t) {}
IoStats CrumbsCannedBus::io_stats_for(uint8_t) const { return IoStats{}; }

void CrumbsCannedBus::apply_write(uint8_t address, const uint8_t *tx_data, size_t tx_len) {
    auto it = devices_.find(address);
    if (it == devices_.end()) {
        return;
    }
    // The write is a fully-encoded CRUMBS request frame; decode it to learn the
    // request (SET_REPLY records which reply to stage; SET_WATCHDOG updates the
    // simulated arming state). Any other control write is simply accepted.
    crumbs_message_t message{};
    if (crumbs_decode_message(tx_data, tx_len, &message, nullptr) != 0) {
        return;
    }
    if (message.opcode == kSetReplyOpcode && message.data_len >= 1) {
        it->second.pending_reply_opcode = message.data[0];
        it->second.has_pending = true;
    } else if (message.opcode == BREAD_OP_SET_WATCHDOG && message.data_len >= 2) {
        it->second.watchdog_timeout_ms =
            static_cast<uint16_t>(message.data[0] | (static_cast<uint16_t>(message.data[1]) << 8));
    }
}

I2cStatus CrumbsCannedBus::emit_reply(uint8_t address, uint8_t *rx_data, size_t rx_len, size_t *rx_received) {
    if (rx_data == nullptr || rx_len == 0) {
        return I2cStatus::failure(I2cError::InvalidArgument, "canned read requires rx buffer");
    }
    auto it = devices_.find(address);
    if (it == devices_.end()) {
        return I2cStatus::failure(I2cError::BusError, "canned bus: no seeded device at address");
    }
    const DeviceState &dev = it->second;
    if (!dev.has_pending) {
        return I2cStatus::failure(I2cError::BusError, "canned bus: no query pending for address");
    }

    crumbs_message_t reply{};
    reply.type_id = dev.type_id;
    reply.opcode = dev.pending_reply_opcode;
    std::vector<uint8_t> payload;
    if (dev.type_id == RLHT_TYPE_ID && dev.pending_reply_opcode == RLHT_OP_GET_STATE) {
        payload = build_rlht_state_payload();
    } else if (dev.type_id == DCMT_TYPE_ID && dev.pending_reply_opcode == DCMT_OP_GET_STATE) {
        payload = build_dcmt_state_payload();
    } else if (dev.pending_reply_opcode == BREAD_OP_GET_WATCHDOG) {
        payload = build_watchdog_payload(dev.watchdog_timeout_ms);
    } else {
        return I2cStatus::failure(I2cError::BusError, "canned bus: no canned response for requested reply");
    }

    reply.data_len = static_cast<uint8_t>(payload.size());
    std::memcpy(reply.data, payload.data(), payload.size());

    // Encode to real wire bytes (header + payload + CRC8), then pad with 0xFF to
    // the requested count — exactly what a fixed-count i2c-dev read of a short
    // CRUMBS frame returns. CrumbsTransport::read then runs crumbs_frame_length +
    // crumbs_decode_message on this, exercising the real trim/decode (bread#97).
    std::array<uint8_t, CRUMBS_MESSAGE_MAX_SIZE> frame{};
    const size_t encoded = crumbs_encode_message(&reply, frame.data(), frame.size());
    if (encoded == 0) {
        return I2cStatus::failure(I2cError::BusError, "canned bus: failed to encode reply");
    }

    for (size_t i = 0; i < rx_len; ++i) {
        rx_data[i] = i < encoded ? frame[i] : static_cast<uint8_t>(0xFFu);
    }
    if (rx_received != nullptr) {
        *rx_received = rx_len;
    }
    return I2cStatus::ok();
}

I2cStatus CrumbsCannedBus::write(uint8_t address, const uint8_t *tx_data, size_t tx_len) {
    if (!opened_) {
        return I2cStatus::failure(I2cError::NotOpen, "bus not open");
    }
    if (tx_len == 0) {
        return I2cStatus::failure(I2cError::InvalidArgument, "write requires tx_len>0");
    }
    apply_write(address, tx_data, tx_len);
    return I2cStatus::ok();
}

I2cStatus CrumbsCannedBus::read(uint8_t address, uint8_t *rx_data, size_t rx_len, size_t *rx_received, uint32_t) {
    if (!opened_) {
        return I2cStatus::failure(I2cError::NotOpen, "bus not open");
    }
    return emit_reply(address, rx_data, rx_len, rx_received);
}

I2cStatus CrumbsCannedBus::write_then_read(uint8_t address, const uint8_t *tx_data, size_t tx_len, uint8_t *rx_data,
                                           size_t rx_len, size_t *rx_received) {
    if (!opened_) {
        return I2cStatus::failure(I2cError::NotOpen, "bus not open");
    }
    if (tx_len > 0) {
        apply_write(address, tx_data, tx_len);
    }
    if (rx_len == 0) {
        if (rx_received != nullptr) {
            *rx_received = 0;
        }
        return I2cStatus::ok();
    }
    return emit_reply(address, rx_data, rx_len, rx_received);
}

}  // namespace anolis_provider_bread::crumbs
