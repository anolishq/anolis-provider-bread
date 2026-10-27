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

uint16_t read_u16_le(const uint8_t *data) {
    return static_cast<uint16_t>(data[0] | (static_cast<uint16_t>(data[1]) << 8));
}

int16_t read_i16_le(const uint8_t *data) { return static_cast<int16_t>(read_u16_le(data)); }

}  // namespace

// RLHT GET_STATE: two warm channels, and the mode, setpoints and on-times the
// device holds; a relay is energized while its channel has on-time. 19 bytes,
// matching rlht_adapter.cpp parse_state. Seeded open-loop with relay 1 on.
std::vector<uint8_t> CrumbsCannedBus::build_rlht_state_payload(const DeviceState &dev) {
    std::vector<uint8_t> p;
    append_u8(p, dev.rlht_mode);
    uint8_t flags = 0;
    if (dev.rlht_on1_ms > 0) {
        flags |= RLHT_FLAG_RELAY1_ON;
    }
    if (dev.rlht_on2_ms > 0) {
        flags |= RLHT_FLAG_RELAY2_ON;
    }
    append_u8(p, flags);
    append_i16_le(p, 250);
    append_i16_le(p, 255);
    append_i16_le(p, dev.rlht_sp1_deci_c);
    append_i16_le(p, dev.rlht_sp2_deci_c);
    append_u16_le(p, dev.rlht_on1_ms);
    append_u16_le(p, dev.rlht_on2_ms);
    append_u16_le(p, dev.rlht_period1_ms);
    append_u16_le(p, dev.rlht_period2_ms);
    append_u8(p, 0);
    return p;
}

// DCMT GET_STATE: the mode and open-loop PWM the device holds, brakes
// released, no e-stop presses. 20 bytes, DCMT_STATE_FIXED_LEN (module major
// 2), matching dcmt_adapter.cpp parse_state. Seeded driving both motors.
std::vector<uint8_t> CrumbsCannedBus::build_dcmt_state_payload(const DeviceState &dev) {
    std::vector<uint8_t> p;
    append_u8(p, dev.dcmt_mode);
    append_i16_le(p, dev.dcmt_m1_pwm);
    append_i16_le(p, dev.dcmt_m2_pwm);
    append_i16_le(p, BREAD_INVALID_I16);
    append_i16_le(p, BREAD_INVALID_I16);
    append_i16_le(p, 0);
    append_i16_le(p, 0);
    append_i16_le(p, BREAD_INVALID_I16);
    append_i16_le(p, BREAD_INVALID_I16);
    append_u8(p, 0);
    append_u8(p, 0);
    append_u8(p, 0);  // estop_count
    return p;
}

namespace {

std::vector<uint8_t> build_watchdog_payload(uint16_t timeout_ms, bool tripped, uint8_t trip_count) {
    std::vector<uint8_t> p;
    append_u8(p, timeout_ms != 0 ? 1 : 0);  // armed
    append_u16_le(p, timeout_ms);
    append_u8(p, tripped ? 1 : 0);
    append_u8(p, trip_count);
    return p;
}

}  // namespace

CrumbsCannedBus::CrumbsCannedBus(std::string bus_path) : bus_path_(std::move(bus_path)) {}

void CrumbsCannedBus::add_device(uint8_t address, uint8_t type_id) { devices_[address].type_id = type_id; }

void CrumbsCannedBus::refuse_next(uint8_t address, uint8_t reason) {
    devices_[address].next_outcomes.emplace_back(CRUMBS_ACK_REFUSED, reason);
}

void CrumbsCannedBus::reject_next(uint8_t address, uint8_t reason) {
    devices_[address].next_outcomes.emplace_back(CRUMBS_ACK_REJECTED, reason);
}

void CrumbsCannedBus::drop_next(uint8_t address, int count) { devices_[address].drop_count += count; }

void CrumbsCannedBus::fail_ack_reads(uint8_t address, int count, int skip) {
    devices_[address].ack_read_skip = skip;
    devices_[address].ack_read_failures = count;
}

int CrumbsCannedBus::writes_of(uint8_t address, uint8_t opcode) const {
    const auto it = devices_.find(address);
    if (it == devices_.end()) {
        return 0;
    }
    const auto found = it->second.writes_by_opcode.find(opcode);
    return found == it->second.writes_by_opcode.end() ? 0 : found->second;
}

void CrumbsCannedBus::trip_watchdog(uint8_t address) {
    auto it = devices_.find(address);
    if (it == devices_.end()) {
        return;
    }
    it->second.watchdog_tripped = true;
    ++it->second.watchdog_trip_count;
}

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
    // simulated arming state and, as on latching firmware, leaves a trip in
    // place; an empty CLEAR_WATCHDOG_TRIP releases it). Any other control write is
    // simply accepted.
    crumbs_message_t message{};
    if (crumbs_decode_message(tx_data, tx_len, &message, nullptr) != 0) {
        return;
    }
    DeviceState &dev = it->second;
    if (message.opcode == kSetReplyOpcode && message.data_len >= 1) {
        dev.pending_reply_opcode = message.data[0];
        dev.has_pending = true;
        return;
    }

    // A command frame: count it for GET_ACK as CRUMBS 0.16 does (the counter
    // moves last), unless a test has it lost, refused or rejected. Only an
    // applied frame is acted on.
    ++dev.writes_by_opcode[message.opcode];
    if (dev.drop_count > 0) {
        --dev.drop_count;
        return;
    }
    uint8_t status = CRUMBS_ACK_APPLIED;
    uint8_t reason = 0;
    if (!dev.next_outcomes.empty()) {
        status = dev.next_outcomes.front().first;
        reason = dev.next_outcomes.front().second;
        dev.next_outcomes.pop_front();
    }
    dev.ack_opcode = message.opcode;
    dev.ack_status = status;
    dev.ack_reason = reason;
    ++dev.ack_counter;
    if (status != CRUMBS_ACK_APPLIED) {
        return;
    }

    if (dev.type_id == DCMT_TYPE_ID && message.opcode == DCMT_OP_SET_OPEN_LOOP && message.data_len >= 4) {
        dev.dcmt_m1_pwm = read_i16_le(message.data);
        dev.dcmt_m2_pwm = read_i16_le(message.data + 2);
    } else if (dev.type_id == DCMT_TYPE_ID && message.opcode == DCMT_OP_SET_MODE && message.data_len >= 1) {
        dev.dcmt_mode = message.data[0];
    } else if (dev.type_id == RLHT_TYPE_ID && message.opcode == RLHT_OP_SET_MODE && message.data_len >= 1) {
        dev.rlht_mode = message.data[0];
    } else if (dev.type_id == RLHT_TYPE_ID && message.opcode == RLHT_OP_SET_SETPOINTS && message.data_len >= 4) {
        dev.rlht_sp1_deci_c = read_i16_le(message.data);
        dev.rlht_sp2_deci_c = read_i16_le(message.data + 2);
    } else if (dev.type_id == RLHT_TYPE_ID && message.opcode == RLHT_OP_SET_OPEN_DUTY && message.data_len >= 2) {
        dev.rlht_on1_ms = static_cast<uint16_t>(dev.rlht_period1_ms * message.data[0] / 100u);
        dev.rlht_on2_ms = static_cast<uint16_t>(dev.rlht_period2_ms * message.data[1] / 100u);
    } else if (message.opcode == BREAD_OP_SET_WATCHDOG && message.data_len >= 2) {
        it->second.watchdog_timeout_ms =
            static_cast<uint16_t>(message.data[0] | (static_cast<uint16_t>(message.data[1]) << 8));
    } else if (message.opcode == BREAD_OP_CLEAR_WATCHDOG_TRIP &&
               message.data_len == BREAD_WATCHDOG_CLEAR_TRIP_PAYLOAD_LEN) {
        // Like the firmware, a clear carrying a payload is rejected and the
        // trip stays set.
        it->second.watchdog_tripped = false;
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
    DeviceState &dev = it->second;
    if (!dev.has_pending) {
        return I2cStatus::failure(I2cError::BusError, "canned bus: no query pending for address");
    }

    crumbs_message_t reply{};
    reply.type_id = dev.type_id;
    reply.opcode = dev.pending_reply_opcode;
    std::vector<uint8_t> payload;
    if (dev.type_id == RLHT_TYPE_ID && dev.pending_reply_opcode == RLHT_OP_GET_STATE) {
        payload = build_rlht_state_payload(dev);
    } else if (dev.type_id == DCMT_TYPE_ID && dev.pending_reply_opcode == DCMT_OP_GET_STATE) {
        payload = build_dcmt_state_payload(dev);
    } else if (dev.pending_reply_opcode == BREAD_OP_GET_WATCHDOG) {
        payload = build_watchdog_payload(dev.watchdog_timeout_ms, dev.watchdog_tripped, dev.watchdog_trip_count);
    } else if (dev.pending_reply_opcode == CRUMBS_CMD_GET_ACK) {
        if (dev.ack_read_skip > 0) {
            --dev.ack_read_skip;
        } else if (dev.ack_read_failures > 0) {
            --dev.ack_read_failures;
            return I2cStatus::failure(I2cError::BusError, "canned bus: injected GET_ACK read failure");
        }
        payload = {dev.ack_counter, dev.ack_opcode, dev.ack_status, dev.ack_reason};
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
