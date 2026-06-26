#include "crumbs/mock_transport.hpp"

/**
 * @file mock_transport.cpp
 * @brief In-memory CRUMBS device simulation for `mock://` sessions.
 *
 * The synthetic GET_STATE payloads here intentionally mirror the byte layouts
 * the device adapters parse (see rlht_adapter.cpp / dcmt_adapter.cpp and the
 * bread-crumbs-contracts headers). Values are plausible, deterministic samples
 * so a mock read yields real declared signals instead of CODE_UNAVAILABLE.
 */

#include <cstddef>

extern "C" {
#include <bread/bread_caps.h>
#include <bread/dcmt_ops.h>
#include <bread/rlht_ops.h>
}

namespace anolis_provider_bread::crumbs {
namespace {

void append_u8(std::vector<uint8_t> &out, uint8_t value) { out.push_back(value); }

void append_u16_le(std::vector<uint8_t> &out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
}

void append_i16_le(std::vector<uint8_t> &out, int16_t value) { append_u16_le(out, static_cast<uint16_t>(value)); }

// RLHT GET_STATE: open-loop, relay 1 energized, two warm channels. 19 bytes,
// matching rlht_adapter.cpp parse_state / rlht_ops.h rlht_get_state.
std::vector<uint8_t> build_rlht_state_payload() {
    std::vector<uint8_t> p;
    append_u8(p, RLHT_MODE_OPEN_LOOP);  // [0]  mode
    append_u8(p, RLHT_FLAG_RELAY1_ON);  // [1]  flags (relay1 on, relay2/estop off)
    append_i16_le(p, 250);              // [2]  t1_deci_c  -> 25.0 C
    append_i16_le(p, 255);              // [4]  t2_deci_c  -> 25.5 C
    append_i16_le(p, 300);              // [6]  sp1_deci_c -> 30.0 C
    append_i16_le(p, 300);              // [8]  sp2_deci_c -> 30.0 C
    append_u16_le(p, 0);                // [10] on1_ms (not exposed)
    append_u16_le(p, 0);                // [12] on2_ms (not exposed)
    append_u16_le(p, 1000);             // [14] period1_ms
    append_u16_le(p, 1000);             // [16] period2_ms
    append_u8(p, 0);                    // [18] tc_select (not exposed)
    return p;
}

// DCMT GET_STATE: open-loop, motors driven, brakes released. 19 bytes, exactly
// DCMT_STATE_FIXED_LEN, matching dcmt_adapter.cpp parse_state / dcmt_ops.h.
std::vector<uint8_t> build_dcmt_state_payload() {
    std::vector<uint8_t> p;
    append_u8(p, DCMT_MODE_OPEN_LOOP);    // [0]  mode
    append_i16_le(p, 100);                // [1]  m1_pwm
    append_i16_le(p, -100);               // [3]  m2_pwm
    append_i16_le(p, BREAD_INVALID_I16);  // [5]  sp1 (sentinel in open-loop)
    append_i16_le(p, BREAD_INVALID_I16);  // [7]  sp2 (sentinel in open-loop)
    append_i16_le(p, 0);                  // [9]  pos1
    append_i16_le(p, 0);                  // [11] pos2
    append_i16_le(p, BREAD_INVALID_I16);  // [13] spd1 (sentinel unless closed-speed)
    append_i16_le(p, BREAD_INVALID_I16);  // [15] spd2 (sentinel unless closed-speed)
    append_u8(p, 0);                      // [17] brakes (both released)
    append_u8(p, 0);                      // [18] estop
    return p;
}

}  // namespace

void MockTransport::add_device(uint8_t address, uint8_t type_id) { device_type_ids_[address] = type_id; }

SessionStatus MockTransport::open(const SessionOptions & /*options*/) {
    open_ = true;
    return SessionStatus::success();
}

void MockTransport::close() noexcept {
    open_ = false;
    pending_reply_opcode_.clear();
}

bool MockTransport::is_open() const { return open_; }

SessionStatus MockTransport::scan(const ScanOptions & /*options*/, std::vector<ScanResult> &out) {
    // Discovery is config-seeded in mock mode, so scan is not used on the read
    // path; report the seeded devices anyway to keep the contract total.
    out.clear();
    for (const auto &[address, type_id] : device_type_ids_) {
        out.push_back(ScanResult{address, true, type_id});
    }
    return SessionStatus::success();
}

SessionStatus MockTransport::send(uint8_t address, const RawFrame &frame) {
    // A SET_REPLY query records which reply the device should return next; any
    // other frame is a control write, which the simulator simply accepts.
    if (frame.opcode == kSetReplyOpcode && !frame.payload.empty()) {
        pending_reply_opcode_[address] = frame.payload[0];
    }
    return SessionStatus::success();
}

SessionStatus MockTransport::read(uint8_t address, RawFrame &frame, uint32_t /*timeout_us*/) {
    const auto pending = pending_reply_opcode_.find(address);
    if (pending == pending_reply_opcode_.end()) {
        return SessionStatus::failure(SessionErrorCode::ReadFailed, "mock transport: no query pending for address");
    }
    const auto type_it = device_type_ids_.find(address);
    if (type_it == device_type_ids_.end()) {
        return SessionStatus::failure(SessionErrorCode::ReadFailed, "mock transport: no seeded device at address");
    }

    const uint8_t type_id = type_it->second;
    const uint8_t reply_opcode = pending->second;

    if (type_id == RLHT_TYPE_ID && reply_opcode == RLHT_OP_GET_STATE) {
        frame.type_id = RLHT_TYPE_ID;
        frame.opcode = RLHT_OP_GET_STATE;
        frame.payload = build_rlht_state_payload();
        return SessionStatus::success();
    }
    if (type_id == DCMT_TYPE_ID && reply_opcode == DCMT_OP_GET_STATE) {
        frame.type_id = DCMT_TYPE_ID;
        frame.opcode = DCMT_OP_GET_STATE;
        frame.payload = build_dcmt_state_payload();
        return SessionStatus::success();
    }

    return SessionStatus::failure(SessionErrorCode::ReadFailed,
                                  "mock transport: no canned response for requested reply");
}

void MockTransport::delay_us(uint32_t /*delay_us*/) {}

}  // namespace anolis_provider_bread::crumbs
