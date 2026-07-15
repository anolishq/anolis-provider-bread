#include "devices/rlht/rlht_adapter.hpp"

/**
 * @file rlht_adapter.cpp
 * @brief RLHT payload decoding and control-call encoding for the BREAD
 * provider.
 */

#include <climits>
#include <cmath>
#include <string>

extern "C" {
#include <bread/rlht_ops.h>
}

namespace anolis_provider_bread::rlht {
namespace {

// ---------------------------------------------------------------------------
// RLHT GET_STATE wire layout is owned by bread-crumbs-contracts
// (rlht_parse_state_payload). The adapter maps the parsed state onto the
// stable ADPP signal surface; on-time and thermocouple-select fields are
// parsed but not exposed as signals.
// ---------------------------------------------------------------------------

constexpr const char *kSigMode = "mode";
constexpr const char *kSigT1 = "t1_c";
constexpr const char *kSigT2 = "t2_c";
constexpr const char *kSigSp1 = "setpoint1_c";
constexpr const char *kSigSp2 = "setpoint2_c";
constexpr const char *kSigPeriod1 = "period1_ms";
constexpr const char *kSigPeriod2 = "period2_ms";
constexpr const char *kSigRelay1 = "relay1_on";
constexpr const char *kSigRelay2 = "relay2_on";
constexpr const char *kSigEstop = "estop";

struct RlhtState {
    uint8_t mode = 0;
    uint8_t flags = 0;
    int16_t t1_dc = 0;
    int16_t t2_dc = 0;
    int16_t sp1_dc = 0;
    int16_t sp2_dc = 0;
    uint16_t period1 = 0;
    uint16_t period2 = 0;
};

bool parse_state(const std::vector<uint8_t> &payload, RlhtState &out) {
    rlht_state_result_t raw{};
    if (payload.size() > UINT8_MAX ||
        rlht_parse_state_payload(payload.data(), static_cast<uint8_t>(payload.size()), &raw) != 0) {
        return false;
    }

    out.mode = raw.mode;
    out.flags = raw.flags;
    out.t1_dc = raw.t1_deci_c;
    out.t2_dc = raw.t2_deci_c;
    out.sp1_dc = raw.sp1_deci_c;
    out.sp2_dc = raw.sp2_deci_c;
    out.period1 = raw.period1_ms;
    out.period2 = raw.period2_ms;
    return true;
}

// [§7.2] Curated default signal set returned for an empty signal_ids request:
// measured temperatures + relay states. Setpoints, periods, mode, and estop are
// excluded (config/target/fault, not routine telemetry).
bool is_default_signal(const char *id) {
    const std::string s(id);
    return s == kSigT1 || s == kSigT2 || s == kSigRelay1 || s == kSigRelay2;
}

bool should_include(const std::vector<std::string> &ids, const char *id) {
    if (ids.empty()) return is_default_signal(id);
    for (const auto &req : ids) {
        if (req == id) return true;
    }
    return false;
}

}  // namespace

AdapterReadResult read_signals(crumbs::Session &session, const inventory::InventoryDevice &device,
                               const std::vector<std::string> &signal_ids) {
    crumbs::RawFrame frame;
    // RLHT exposes its readable state through one aggregate frame, so the
    // adapter always fetches the full payload and filters after decoding.
    const crumbs::SessionStatus status =
        session.query_read(static_cast<uint8_t>(device.address), RLHT_OP_GET_STATE, frame);

    if (!status.ok()) {
        return read_error_from_session(status, "RLHT GET_STATE");
    }

    if (frame.type_id != RLHT_TYPE_ID || frame.opcode != RLHT_OP_GET_STATE) {
        return {false, anolis::deviceprovider::v1::Status::CODE_INTERNAL, "unexpected RLHT GET_STATE frame header", {}};
    }

    RlhtState s;
    if (!parse_state(frame.payload, s)) {
        return {false,
                anolis::deviceprovider::v1::Status::CODE_INTERNAL,
                "RLHT GET_STATE payload too short or malformed",
                {}};
    }

    const std::string mode_str = (s.mode == RLHT_MODE_OPEN_LOOP) ? "open_loop" : "closed_loop";

    AdapterReadResult result;
    result.ok = true;
    auto &vals = result.values;

    // Sentinel-eligible deci-C fields: the firmware sends BREAD_INVALID_I16
    // for an open thermocouple (isnan on the slice). Dividing the sentinel
    // yields -3276.8 C with quality OK (#109) — emit QUALITY_FAULT with a
    // placeholder 0.0 instead, so consumers keyed on quality never act on a
    // fabricated temperature.
    const auto deci_c_signal = [](const char *id, int16_t deci_c) {
        if (!BREAD_IS_VALID_I16(deci_c)) {
            return make_signal_value(id, make_double_val(0.0), anolis::deviceprovider::v1::SignalValue::QUALITY_FAULT);
        }
        return make_signal_value(id, make_double_val(deci_c / 10.0));
    };

    if (should_include(signal_ids, kSigMode)) vals.push_back(make_signal_value(kSigMode, make_string_val(mode_str)));
    if (should_include(signal_ids, kSigT1)) vals.push_back(deci_c_signal(kSigT1, s.t1_dc));
    if (should_include(signal_ids, kSigT2)) vals.push_back(deci_c_signal(kSigT2, s.t2_dc));
    if (should_include(signal_ids, kSigSp1)) vals.push_back(deci_c_signal(kSigSp1, s.sp1_dc));
    if (should_include(signal_ids, kSigSp2)) vals.push_back(deci_c_signal(kSigSp2, s.sp2_dc));
    if (should_include(signal_ids, kSigPeriod1))
        vals.push_back(make_signal_value(kSigPeriod1, make_uint64_val(s.period1)));
    if (should_include(signal_ids, kSigPeriod2))
        vals.push_back(make_signal_value(kSigPeriod2, make_uint64_val(s.period2)));
    if (should_include(signal_ids, kSigRelay1))
        vals.push_back(make_signal_value(kSigRelay1, make_bool_val((s.flags & RLHT_FLAG_RELAY1_ON) != 0u)));
    if (should_include(signal_ids, kSigRelay2))
        vals.push_back(make_signal_value(kSigRelay2, make_bool_val((s.flags & RLHT_FLAG_RELAY2_ON) != 0u)));
    if (should_include(signal_ids, kSigEstop))
        vals.push_back(make_signal_value(kSigEstop, make_bool_val((s.flags & RLHT_FLAG_ESTOP) != 0u)));

    return result;
}

// §8.3: validation + frame encoding only — no session, so the handler can
// validate arguments before checking hardware availability.
AdapterCallResult build_frame(uint32_t function_id, const ValueMap &args, crumbs::RawFrame &frame) {
    frame.type_id = RLHT_TYPE_ID;

    switch (function_id) {
        // Function IDs must stay aligned with the capability definitions published
        // by the inventory layer for RLHT devices.
        case 1: {  // set_mode
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
            append_u8(frame.payload, mode == "open_loop" ? RLHT_MODE_OPEN_LOOP : RLHT_MODE_CLOSED_LOOP);
            break;
        }
        case 2: {  // set_setpoints
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
            if (!std::isfinite(sp1) || !std::isfinite(sp2)) {  // §8.3 [L2]: non-finite first
                return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                        "setpoint values must be finite (not NaN or +/-Inf)"};
            }
            constexpr double kMinSetpointC = static_cast<double>(INT16_MIN) / 10.0;
            constexpr double kMaxSetpointC = static_cast<double>(INT16_MAX) / 10.0;
            if (sp1 < kMinSetpointC || sp1 > kMaxSetpointC || sp2 < kMinSetpointC || sp2 > kMaxSetpointC) {
                return {false, anolis::deviceprovider::v1::Status::CODE_OUT_OF_RANGE,  // §8.3 [L2]
                        "setpoint values must be in [-3276.8, 3276.7] C"};
            }
            const auto sp1_deci = std::llround(sp1 * 10.0);
            const auto sp2_deci = std::llround(sp2 * 10.0);
            if (sp1_deci < INT16_MIN || sp1_deci > INT16_MAX || sp2_deci < INT16_MIN || sp2_deci > INT16_MAX) {
                return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                        "setpoint conversion out of int16 range"};
            }
            frame.opcode = RLHT_OP_SET_SETPOINTS;
            append_i16_le(frame.payload, static_cast<int16_t>(sp1_deci));
            append_i16_le(frame.payload, static_cast<int16_t>(sp2_deci));
            break;
        }
        case 3: {  // set_pid_x10
            uint64_t kp1 = 0, ki1 = 0, kd1 = 0, kp2 = 0, ki2 = 0, kd2 = 0;
            if (!get_arg_uint64(args, "kp1_x10", kp1) || !get_arg_uint64(args, "ki1_x10", ki1) ||
                !get_arg_uint64(args, "kd1_x10", kd1) || !get_arg_uint64(args, "kp2_x10", kp2) ||
                !get_arg_uint64(args, "ki2_x10", ki2) || !get_arg_uint64(args, "kd2_x10", kd2)) {
                return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                        "missing or invalid PID gain args (kp1/ki1/kd1/kp2/ki2/kd2_x10, "
                        "all uint64)"};
            }
            if (kp1 > 255u || ki1 > 255u || kd1 > 255u || kp2 > 255u || ki2 > 255u || kd2 > 255u) {
                return {false, anolis::deviceprovider::v1::Status::CODE_OUT_OF_RANGE,
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
        case 4: {  // set_periods_ms
            uint64_t p1 = 0, p2 = 0;
            if (!get_arg_uint64(args, "period1_ms", p1) || !get_arg_uint64(args, "period2_ms", p2)) {
                return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                        "missing or invalid args: period1_ms, period2_ms (uint64)"};
            }
            if (p1 > 65535u || p2 > 65535u) {
                return {false, anolis::deviceprovider::v1::Status::CODE_OUT_OF_RANGE,
                        "period values must be in [0, 65535] ms"};
            }
            frame.opcode = RLHT_OP_SET_PERIODS;
            append_u16_le(frame.payload, static_cast<uint16_t>(p1));
            append_u16_le(frame.payload, static_cast<uint16_t>(p2));
            break;
        }
        case 5: {  // set_tc_select
            uint64_t tc1 = 0, tc2 = 0;
            if (!get_arg_uint64(args, "tc1_index", tc1) || !get_arg_uint64(args, "tc2_index", tc2)) {
                return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                        "missing or invalid args: tc1_index, tc2_index (uint64)"};
            }
            if (tc1 > 255u || tc2 > 255u) {
                return {false, anolis::deviceprovider::v1::Status::CODE_OUT_OF_RANGE,
                        "tc_index values must be in [0, 255]"};
            }
            frame.opcode = RLHT_OP_SET_TC_SELECT;
            append_u8(frame.payload, static_cast<uint8_t>(tc1));
            append_u8(frame.payload, static_cast<uint8_t>(tc2));
            break;
        }
        case 6: {  // set_open_duty_pct
            uint64_t d1 = 0, d2 = 0;
            if (!get_arg_uint64(args, "duty1_pct", d1) || !get_arg_uint64(args, "duty2_pct", d2)) {
                return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                        "missing or invalid args: duty1_pct, duty2_pct (uint64)"};
            }
            if (d1 > 100u || d2 > 100u) {
                return {false, anolis::deviceprovider::v1::Status::CODE_OUT_OF_RANGE,
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

    // RLHT setters are fire-and-forget at this layer; confirmation is observed
    // on the next state read rather than through a dedicated ACK payload.
    return {true, anolis::deviceprovider::v1::Status::CODE_OK, "ok"};
}

AdapterCallResult transmit(crumbs::Session &session, const inventory::InventoryDevice &device,
                           const crumbs::RawFrame &frame) {
    const auto addr = static_cast<uint8_t>(device.address);
    const crumbs::SessionStatus send_status = session.send(addr, frame);
    if (!send_status.ok()) {
        return call_error_from_session(send_status, "RLHT SET command");
    }
    return {true, anolis::deviceprovider::v1::Status::CODE_OK, "ok"};
}

AdapterCallResult call(crumbs::Session &session, const inventory::InventoryDevice &device, uint32_t function_id,
                       const ValueMap &args) {
    crumbs::RawFrame frame;
    AdapterCallResult built = build_frame(function_id, args, frame);
    if (!built.ok) {
        return built;
    }
    return transmit(session, device, frame);
}

}  // namespace anolis_provider_bread::rlht
