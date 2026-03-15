#include "devices/dcmt/dcmt_adapter.hpp"

#include <string>

extern "C" {
#include <bread/dcmt_ops.h>
}

namespace anolis_provider_bread::dcmt {
namespace {

// ---------------------------------------------------------------------------
// DCMT GET_STATE payload layout (mode-dependent)
//
// Open-loop (DCMT_MODE_OPEN_LOOP = 0x00), 7 bytes:
//   [0]   mode     u8  (= 0x00)
//   [1-2] target1  i16 LE
//   [3-4] target2  i16 LE
//   [5]   brakes   u8
//   [6]   estop    u8
//   value1 = target1, value2 = target2 (no encoder feedback in open-loop)
//
// Closed-loop (DCMT_MODE_CLOSED_POSITION = 0x01 or DCMT_MODE_CLOSED_SPEED = 0x02), 11 bytes:
//   [0]   mode     u8  (= 0x01 or 0x02)
//   [1-2] target1  i16 LE
//   [3-4] target2  i16 LE
//   [5-6] value1   i16 LE  (measured encoder/speed value)
//   [7-8] value2   i16 LE
//   [9]   brakes   u8
//   [10]  estop    u8
// ---------------------------------------------------------------------------

constexpr std::size_t kMinOpenLoopPayloadBytes   = 7u;
constexpr std::size_t kMinClosedLoopPayloadBytes = 11u;

constexpr const char *kSigMode        = "mode";
constexpr const char *kSigMotor1Tgt   = "motor1_target";
constexpr const char *kSigMotor2Tgt   = "motor2_target";
constexpr const char *kSigMotor1Val   = "motor1_value";
constexpr const char *kSigMotor2Val   = "motor2_value";
constexpr const char *kSigMotor1Brake = "motor1_brake";
constexpr const char *kSigMotor2Brake = "motor2_brake";
constexpr const char *kSigEstop       = "estop";

struct DcmtState {
    uint8_t mode    = 0;
    int16_t target1 = 0;
    int16_t target2 = 0;
    int16_t value1  = 0;  // equals target1 in open-loop
    int16_t value2  = 0;  // equals target2 in open-loop
    uint8_t brakes  = 0;
    uint8_t estop   = 0;
};

bool parse_state(const std::vector<uint8_t> &payload, DcmtState &out) {
    if (!read_u8(payload, 0u, out.mode)) return false;

    if (out.mode == DCMT_MODE_OPEN_LOOP) {
        if (payload.size() < kMinOpenLoopPayloadBytes) return false;
        if (!read_i16_le(payload, 1u, out.target1)) return false;
        if (!read_i16_le(payload, 3u, out.target2)) return false;
        out.value1 = out.target1;
        out.value2 = out.target2;
        if (!read_u8(payload, 5u, out.brakes)) return false;
        if (!read_u8(payload, 6u, out.estop))  return false;
    } else {
        if (payload.size() < kMinClosedLoopPayloadBytes) return false;
        if (!read_i16_le(payload, 1u, out.target1)) return false;
        if (!read_i16_le(payload, 3u, out.target2)) return false;
        if (!read_i16_le(payload, 5u, out.value1))  return false;
        if (!read_i16_le(payload, 7u, out.value2))  return false;
        if (!read_u8(payload, 9u,  out.brakes))     return false;
        if (!read_u8(payload, 10u, out.estop))      return false;
    }
    return true;
}

const char *mode_to_string(uint8_t mode) {
    switch (mode) {
    case DCMT_MODE_OPEN_LOOP:       return "open_loop";
    case DCMT_MODE_CLOSED_POSITION: return "closed_position";
    case DCMT_MODE_CLOSED_SPEED:    return "closed_speed";
    default:                        return "unknown";
    }
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
    const crumbs::SessionStatus status =
        session.query_read(static_cast<uint8_t>(device.address),
                           DCMT_OP_GET_STATE,
                           frame);

    if (!status.ok()) {
        return read_error_from_session(status, "DCMT GET_STATE");
    }

    if (frame.type_id != DCMT_TYPE_ID || frame.opcode != DCMT_OP_GET_STATE) {
        return {false,
                anolis::deviceprovider::v1::Status::CODE_INTERNAL,
                "unexpected DCMT GET_STATE frame header"};
    }

    DcmtState s;
    if (!parse_state(frame.payload, s)) {
        return {false,
                anolis::deviceprovider::v1::Status::CODE_INTERNAL,
                "DCMT GET_STATE payload too short or malformed"};
    }

    AdapterReadResult result;
    result.ok = true;
    auto &vals = result.values;

    if (should_include(signal_ids, kSigMode))
        vals.push_back(make_signal_value(kSigMode, make_string_val(mode_to_string(s.mode))));
    if (should_include(signal_ids, kSigMotor1Tgt))
        vals.push_back(make_signal_value(kSigMotor1Tgt, make_int64_val(s.target1)));
    if (should_include(signal_ids, kSigMotor2Tgt))
        vals.push_back(make_signal_value(kSigMotor2Tgt, make_int64_val(s.target2)));
    if (should_include(signal_ids, kSigMotor1Val))
        vals.push_back(make_signal_value(kSigMotor1Val, make_int64_val(s.value1)));
    if (should_include(signal_ids, kSigMotor2Val))
        vals.push_back(make_signal_value(kSigMotor2Val, make_int64_val(s.value2)));
    if (should_include(signal_ids, kSigMotor1Brake))
        vals.push_back(make_signal_value(kSigMotor1Brake, make_bool_val((s.brakes & 0x01u) != 0u)));
    if (should_include(signal_ids, kSigMotor2Brake))
        vals.push_back(make_signal_value(kSigMotor2Brake, make_bool_val((s.brakes & 0x02u) != 0u)));
    if (should_include(signal_ids, kSigEstop))
        vals.push_back(make_signal_value(kSigEstop, make_bool_val(s.estop != 0u)));

    return result;
}

AdapterCallResult call(crumbs::Session &session,
                       const inventory::InventoryDevice &device,
                       uint32_t function_id,
                       const ValueMap &args) {
    const auto addr = static_cast<uint8_t>(device.address);
    crumbs::RawFrame frame;
    frame.type_id = DCMT_TYPE_ID;

    switch (function_id) {
    case 1: { // set_open_loop
        int64_t m1 = 0, m2 = 0;
        if (!get_arg_int64(args, "motor1_pwm", m1)) {
            return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                    "missing or invalid arg: motor1_pwm (int64)"};
        }
        if (!get_arg_int64(args, "motor2_pwm", m2)) {
            return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                    "missing or invalid arg: motor2_pwm (int64)"};
        }
        if (m1 < INT16_MIN || m1 > INT16_MAX || m2 < INT16_MIN || m2 > INT16_MAX) {
            return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                    "motor PWM values must be in [-32768, 32767]"};
        }
        frame.opcode = DCMT_OP_SET_OPEN_LOOP;
        append_i16_le(frame.payload, static_cast<int16_t>(m1));
        append_i16_le(frame.payload, static_cast<int16_t>(m2));
        break;
    }
    case 2: { // set_brake
        bool b1 = false, b2 = false;
        if (!get_arg_bool(args, "motor1_brake", b1)) {
            return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                    "missing or invalid arg: motor1_brake (bool)"};
        }
        if (!get_arg_bool(args, "motor2_brake", b2)) {
            return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                    "missing or invalid arg: motor2_brake (bool)"};
        }
        frame.opcode = DCMT_OP_SET_BRAKE;
        append_u8(frame.payload, b1 ? 1u : 0u);
        append_u8(frame.payload, b2 ? 1u : 0u);
        break;
    }
    case 3: { // set_mode
        std::string mode;
        if (!get_arg_string(args, "mode", mode)) {
            return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                    "missing or invalid arg: mode (string)"};
        }
        uint8_t mode_byte = 0;
        if (mode == "open_loop") {
            mode_byte = DCMT_MODE_OPEN_LOOP;
        } else if (mode == "closed_position") {
            mode_byte = DCMT_MODE_CLOSED_POSITION;
        } else if (mode == "closed_speed") {
            mode_byte = DCMT_MODE_CLOSED_SPEED;
        } else {
            return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                    "mode must be 'open_loop', 'closed_position', or 'closed_speed'"};
        }
        frame.opcode = DCMT_OP_SET_MODE;
        append_u8(frame.payload, mode_byte);
        break;
    }
    case 4: { // set_setpoint
        int64_t t1 = 0, t2 = 0;
        if (!get_arg_int64(args, "motor1_target", t1)) {
            return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                    "missing or invalid arg: motor1_target (int64)"};
        }
        if (!get_arg_int64(args, "motor2_target", t2)) {
            return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                    "missing or invalid arg: motor2_target (int64)"};
        }
        if (t1 < INT16_MIN || t1 > INT16_MAX || t2 < INT16_MIN || t2 > INT16_MAX) {
            return {false, anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT,
                    "motor target values must be in [-32768, 32767]"};
        }
        frame.opcode = DCMT_OP_SET_SETPOINT;
        append_i16_le(frame.payload, static_cast<int16_t>(t1));
        append_i16_le(frame.payload, static_cast<int16_t>(t2));
        break;
    }
    case 5: { // set_pid_x10
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
        frame.opcode = DCMT_OP_SET_PID;
        append_u8(frame.payload, static_cast<uint8_t>(kp1));
        append_u8(frame.payload, static_cast<uint8_t>(ki1));
        append_u8(frame.payload, static_cast<uint8_t>(kd1));
        append_u8(frame.payload, static_cast<uint8_t>(kp2));
        append_u8(frame.payload, static_cast<uint8_t>(ki2));
        append_u8(frame.payload, static_cast<uint8_t>(kd2));
        break;
    }
    default:
        return {false, anolis::deviceprovider::v1::Status::CODE_NOT_FOUND,
                "unknown DCMT function_id " + std::to_string(function_id)};
    }

    const crumbs::SessionStatus send_status = session.send(addr, frame);
    if (!send_status.ok()) {
        return call_error_from_session(send_status, "DCMT SET command");
    }

    return {true, anolis::deviceprovider::v1::Status::CODE_OK, "ok"};
}

} // namespace anolis_provider_bread::dcmt
