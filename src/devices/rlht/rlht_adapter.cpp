#include "devices/rlht/rlht_adapter.hpp"

/**
 * @file rlht_adapter.cpp
 * @brief RLHT payload decoding and control-call encoding for the BREAD provider.
 */

#include <string>

extern "C" {
#include <bread/rlht_ops.h>
}

namespace anolis_provider_bread::rlht {
namespace {

// ---------------------------------------------------------------------------
// RLHT GET_STATE payload layout (19 bytes minimum)
//   [0]     mode       u8
//   [1]     flags      u8
//   [2-3]   t1_deci_c  i16 LE
//   [4-5]   t2_deci_c  i16 LE
//   [6-7]   sp1_deci_c i16 LE
//   [8-9]   sp2_deci_c i16 LE
//   [10-11] on1_ms     u16 LE  (not exposed as signal)
//   [12-13] on2_ms     u16 LE  (not exposed as signal)
//   [14-15] period1_ms u16 LE
//   [16-17] period2_ms u16 LE
//   [18]    tc_select  u8      (not exposed as signal)
// ---------------------------------------------------------------------------

constexpr std::size_t kMinStatePayloadBytes = 19u;

constexpr const char *kSigMode    = "mode";
constexpr const char *kSigT1      = "t1_c";
constexpr const char *kSigT2      = "t2_c";
constexpr const char *kSigSp1     = "setpoint1_c";
constexpr const char *kSigSp2     = "setpoint2_c";
constexpr const char *kSigPeriod1 = "period1_ms";
constexpr const char *kSigPeriod2 = "period2_ms";
constexpr const char *kSigRelay1  = "relay1_on";
constexpr const char *kSigRelay2  = "relay2_on";
constexpr const char *kSigEstop   = "estop";

struct RlhtState {
    uint8_t  mode    = 0;
    uint8_t  flags   = 0;
    int16_t  t1_dc   = 0;
    int16_t  t2_dc   = 0;
    int16_t  sp1_dc  = 0;
    int16_t  sp2_dc  = 0;
    uint16_t period1 = 0;
    uint16_t period2 = 0;
};

bool parse_state(const std::vector<uint8_t> &payload, RlhtState &out) {
    if (payload.size() < kMinStatePayloadBytes) return false;
    if (!read_u8(payload, 0u, out.mode))         return false;
    if (!read_u8(payload, 1u, out.flags))         return false;
    if (!read_i16_le(payload, 2u, out.t1_dc))    return false;
    if (!read_i16_le(payload, 4u, out.t2_dc))    return false;
    if (!read_i16_le(payload, 6u, out.sp1_dc))   return false;
    if (!read_i16_le(payload, 8u, out.sp2_dc))   return false;
    // skip on1_ms[10-11] and on2_ms[12-13]
    if (!read_u16_le(payload, 14u, out.period1)) return false;
    if (!read_u16_le(payload, 16u, out.period2)) return false;
    return true;
}

bool should_include(const std::vector<std::string> &ids, const char *id) {
    if (ids.empty()) return true;
    for (const auto &req : ids) {
        if (req == id) return true;
    }
    return false;
}

} // namespace

AdapterReadResult read_signals(crumbs::Session &session,
                               const inventory::InventoryDevice &device,
                               const std::vector<std::string> &signal_ids) {
    crumbs::RawFrame frame;
    // RLHT exposes its readable state through one aggregate frame, so the
    // adapter always fetches the full payload and filters after decoding.
    const crumbs::SessionStatus status =
        session.query_read(static_cast<uint8_t>(device.address),
                           RLHT_OP_GET_STATE,
                           frame);

    if (!status.ok()) {
        return read_error_from_session(status, "RLHT GET_STATE");
    }

    if (frame.type_id != RLHT_TYPE_ID || frame.opcode != RLHT_OP_GET_STATE) {
        return {false,
                anolis::deviceprovider::v1::Status::CODE_INTERNAL,
                "unexpected RLHT GET_STATE frame header",
                {}};
    }

    RlhtState s;
    if (!parse_state(frame.payload, s)) {
        return {false,
                anolis::deviceprovider::v1::Status::CODE_INTERNAL,
                "RLHT GET_STATE payload too short or malformed",
                {}};
    }

    const std::string mode_str =
        (s.mode == RLHT_MODE_OPEN_LOOP) ? "open_loop" : "closed_loop";

    AdapterReadResult result;
    result.ok = true;
    auto &vals = result.values;

    if (should_include(signal_ids, kSigMode))
        vals.push_back(make_signal_value(kSigMode, make_string_val(mode_str)));
    if (should_include(signal_ids, kSigT1))
        vals.push_back(make_signal_value(kSigT1, make_double_val(s.t1_dc / 10.0)));
    if (should_include(signal_ids, kSigT2))
        vals.push_back(make_signal_value(kSigT2, make_double_val(s.t2_dc / 10.0)));
    if (should_include(signal_ids, kSigSp1))
        vals.push_back(make_signal_value(kSigSp1, make_double_val(s.sp1_dc / 10.0)));
    if (should_include(signal_ids, kSigSp2))
        vals.push_back(make_signal_value(kSigSp2, make_double_val(s.sp2_dc / 10.0)));
    if (should_include(signal_ids, kSigPeriod1))
        vals.push_back(make_signal_value(kSigPeriod1, make_uint64_val(s.period1)));
    if (should_include(signal_ids, kSigPeriod2))
        vals.push_back(make_signal_value(kSigPeriod2, make_uint64_val(s.period2)));
    if (should_include(signal_ids, kSigRelay1))
        vals.push_back(make_signal_value(kSigRelay1,
            make_bool_val((s.flags & RLHT_FLAG_RELAY1_ON) != 0u)));
    if (should_include(signal_ids, kSigRelay2))
        vals.push_back(make_signal_value(kSigRelay2,
            make_bool_val((s.flags & RLHT_FLAG_RELAY2_ON) != 0u)));
    if (should_include(signal_ids, kSigEstop))
        vals.push_back(make_signal_value(kSigEstop,
            make_bool_val((s.flags & RLHT_FLAG_ESTOP) != 0u)));

    return result;
}

AdapterCallResult call(crumbs::Session &session,
                       const inventory::InventoryDevice &device,
                       uint32_t function_id,
                       const ValueMap &args) {
    const auto addr = static_cast<uint8_t>(device.address);
    crumbs::RawFrame frame;
    frame.type_id = RLHT_TYPE_ID;

    switch (function_id) {
    // Function IDs must stay aligned with the capability definitions published
    // by the inventory layer for RLHT devices.
    case 1: { // set_mode
        std::string mode;
        if (!get_arg_string(args, "mode", mode)) {
            return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                    "missing or invalid arg: mode (string)"};
        }
        if (mode != "closed_loop" && mode != "open_loop") {
            return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                    "mode must be 'closed_loop' or 'open_loop'"};
        }
        frame.opcode = RLHT_OP_SET_MODE;
        append_u8(frame.payload,
                  mode == "open_loop" ? RLHT_MODE_OPEN_LOOP : RLHT_MODE_CLOSED_LOOP);
        break;
    }
    case 2: { // set_setpoints
        double sp1 = 0.0;
        double sp2 = 0.0;
        if (!get_arg_double(args, "setpoint1_c", sp1)) {
            return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                    "missing or invalid arg: setpoint1_c (double)"};
        }
        if (!get_arg_double(args, "setpoint2_c", sp2)) {
            return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                    "missing or invalid arg: setpoint2_c (double)"};
        }
        frame.opcode = RLHT_OP_SET_SETPOINTS;
        append_i16_le(frame.payload, static_cast<int16_t>(sp1 * 10.0));
        append_i16_le(frame.payload, static_cast<int16_t>(sp2 * 10.0));
        break;
    }
    case 3: { // set_pid_x10
        uint64_t kp1 = 0, ki1 = 0, kd1 = 0, kp2 = 0, ki2 = 0, kd2 = 0;
        if (!get_arg_uint64(args, "kp1_x10", kp1) ||
            !get_arg_uint64(args, "ki1_x10", ki1) ||
            !get_arg_uint64(args, "kd1_x10", kd1) ||
            !get_arg_uint64(args, "kp2_x10", kp2) ||
            !get_arg_uint64(args, "ki2_x10", ki2) ||
            !get_arg_uint64(args, "kd2_x10", kd2)) {
            return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                    "missing or invalid PID gain args (kp1/ki1/kd1/kp2/ki2/kd2_x10, all uint64)"};
        }
        if (kp1 > 255u || ki1 > 255u || kd1 > 255u ||
            kp2 > 255u || ki2 > 255u || kd2 > 255u) {
            return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                    "PID gain values must be in [0, 255]"};
        }
        frame.opcode = RLHT_OP_SET_PID;
        append_u8(frame.payload, static_cast<uint8_t>(kp1));
        append_u8(frame.payload, static_cast<uint8_t>(ki1));
        append_u8(frame.payload, static_cast<uint8_t>(kd1));
        append_u8(frame.payload, static_cast<uint8_t>(kp2));
        append_u8(frame.payload, static_cast<uint8_t>(ki2));
        append_u8(frame.payload, static_cast<uint8_t>(kd2));
        break;
    }
    case 4: { // set_periods_ms
        uint64_t p1 = 0, p2 = 0;
        if (!get_arg_uint64(args, "period1_ms", p1) ||
            !get_arg_uint64(args, "period2_ms", p2)) {
            return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                    "missing or invalid args: period1_ms, period2_ms (uint64)"};
        }
        if (p1 > 65535u || p2 > 65535u) {
            return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                    "period values must be in [0, 65535] ms"};
        }
        frame.opcode = RLHT_OP_SET_PERIODS;
        append_u16_le(frame.payload, static_cast<uint16_t>(p1));
        append_u16_le(frame.payload, static_cast<uint16_t>(p2));
        break;
    }
    case 5: { // set_tc_select
        uint64_t tc1 = 0, tc2 = 0;
        if (!get_arg_uint64(args, "tc1_index", tc1) ||
            !get_arg_uint64(args, "tc2_index", tc2)) {
            return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                    "missing or invalid args: tc1_index, tc2_index (uint64)"};
        }
        if (tc1 > 255u || tc2 > 255u) {
            return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                    "tc_index values must be in [0, 255]"};
        }
        frame.opcode = RLHT_OP_SET_TC_SELECT;
        append_u8(frame.payload, static_cast<uint8_t>(tc1));
        append_u8(frame.payload, static_cast<uint8_t>(tc2));
        break;
    }
    case 6: { // set_open_duty_pct
        uint64_t d1 = 0, d2 = 0;
        if (!get_arg_uint64(args, "duty1_pct", d1) ||
            !get_arg_uint64(args, "duty2_pct", d2)) {
            return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                    "missing or invalid args: duty1_pct, duty2_pct (uint64)"};
        }
        if (d1 > 100u || d2 > 100u) {
            return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                    "duty_pct values must be in [0, 100]"};
        }
        frame.opcode = RLHT_OP_SET_OPEN_DUTY;
        append_u8(frame.payload, static_cast<uint8_t>(d1));
        append_u8(frame.payload, static_cast<uint8_t>(d2));
        break;
    }
    default:
        return {false, anolis::deviceprovider::v1::Status::CODE_NOT_FOUND,
                "unknown RLHT function_id " + std::to_string(function_id)};
    }

    const crumbs::SessionStatus send_status = session.send(addr, frame);
    if (!send_status.ok()) {
        return call_error_from_session(send_status, "RLHT SET command");
    }

    // RLHT setters are fire-and-forget at this layer; confirmation is observed
    // on the next state read rather than through a dedicated ACK payload.
    return {true, anolis::deviceprovider::v1::Status::CODE_OK, "ok"};
}

} // namespace anolis_provider_bread::rlht
