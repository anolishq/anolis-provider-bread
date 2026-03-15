#include "devices/dcmt/dcmt_adapter.hpp"

#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "crumbs/session.hpp"
#include "devices/common/inventory.hpp"

extern "C" {
#include <bread/dcmt_ops.h>
}

namespace anolis_provider_bread::dcmt {
namespace {

// ---------------------------------------------------------------------------
// Minimal scripted transport (same pattern as rlht_adapter_test).
// ---------------------------------------------------------------------------

class AdapterTestTransport final : public crumbs::Transport {
public:
    std::vector<std::pair<uint8_t, crumbs::RawFrame>> sent_frames;
    std::map<uint8_t, crumbs::RawFrame> read_replies;
    crumbs::SessionErrorCode read_error = crumbs::SessionErrorCode::Ok;
    crumbs::SessionErrorCode send_error = crumbs::SessionErrorCode::Ok;

    crumbs::SessionStatus open(const crumbs::SessionOptions &) override {
        open_ = true;
        return crumbs::SessionStatus::success();
    }
    void close() noexcept override { open_ = false; }
    bool is_open() const override { return open_; }
    void delay_us(uint32_t) override {}
    crumbs::SessionStatus scan(const crumbs::ScanOptions &,
                               std::vector<crumbs::ScanResult> &) override {
        return crumbs::SessionStatus::success();
    }

    crumbs::SessionStatus send(uint8_t address,
                               const crumbs::RawFrame &frame) override {
        sent_frames.push_back({address, frame});
        if (frame.opcode == crumbs::kSetReplyOpcode && !frame.payload.empty()) {
            pending_opcode_ = frame.payload[0];
        }
        if (send_error != crumbs::SessionErrorCode::Ok) {
            return crumbs::SessionStatus::failure(send_error, "scripted send error");
        }
        return crumbs::SessionStatus::success();
    }

    crumbs::SessionStatus read(uint8_t, crumbs::RawFrame &out,
                               uint32_t) override {
        if (read_error != crumbs::SessionErrorCode::Ok) {
            return crumbs::SessionStatus::failure(read_error, "scripted read error");
        }
        const auto it = read_replies.find(pending_opcode_);
        if (it == read_replies.end()) {
            return crumbs::SessionStatus::failure(crumbs::SessionErrorCode::ReadFailed,
                                                   "no scripted reply for opcode");
        }
        out = it->second;
        return crumbs::SessionStatus::success();
    }

private:
    bool open_ = false;
    uint8_t pending_opcode_ = 0;
};

// ---------------------------------------------------------------------------
// Payload builders
// ---------------------------------------------------------------------------

static std::vector<uint8_t> make_open_loop_payload(
    int16_t target1, int16_t target2, uint8_t brakes, uint8_t estop) {
    std::vector<uint8_t> p;
    append_u8(p, DCMT_MODE_OPEN_LOOP);
    append_i16_le(p, target1);
    append_i16_le(p, target2);
    append_u8(p, brakes);
    append_u8(p, estop);
    return p;
}

static std::vector<uint8_t> make_closed_loop_payload(
    uint8_t mode, int16_t target1, int16_t target2,
    int16_t value1, int16_t value2, uint8_t brakes, uint8_t estop) {
    std::vector<uint8_t> p;
    append_u8(p, mode);
    append_i16_le(p, target1);
    append_i16_le(p, target2);
    append_i16_le(p, value1);
    append_i16_le(p, value2);
    append_u8(p, brakes);
    append_u8(p, estop);
    return p;
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

struct DcmtAdapterTest : public ::testing::Test {
    AdapterTestTransport transport;
    crumbs::Session session{transport, []{
        crumbs::SessionOptions opts;
        opts.bus_path = "/dev/i2c-test";
        return opts;
    }()};

    inventory::InventoryDevice device;

    void SetUp() override {
        ASSERT_TRUE(session.open().ok());
        device.address = 0x09;
        device.type = DeviceType::Dcmt;
    }

    void script_state_reply(std::vector<uint8_t> payload) {
        transport.read_replies[DCMT_OP_GET_STATE] =
            crumbs::RawFrame{DCMT_TYPE_ID, DCMT_OP_GET_STATE, std::move(payload)};
    }

    static ValueMap make_args(
        std::initializer_list<std::pair<std::string, anolis::deviceprovider::v1::Value>> pairs) {
        ValueMap m;
        for (const auto &kv : pairs) {
            m[kv.first] = kv.second;
        }
        return m;
    }
};

// ---------------------------------------------------------------------------
// read_signals — open-loop layout
// ---------------------------------------------------------------------------

TEST_F(DcmtAdapterTest, ReadSignals_OpenLoop_ReturnsAllSignals) {
    // target1=200, target2=-150, brakes=motor1 on, estop=0
    script_state_reply(make_open_loop_payload(200, -150, 0x01, 0x00));

    const auto result = read_signals(session, device, {});

    ASSERT_TRUE(result.ok) << result.error_message;
    ASSERT_EQ(result.values.size(), 8u);

    const auto find = [&](const std::string &id) -> const anolis::deviceprovider::v1::SignalValue * {
        for (const auto &sv : result.values) {
            if (sv.signal_id() == id) return &sv;
        }
        return nullptr;
    };

    const auto *mode_sv = find("mode");
    ASSERT_NE(mode_sv, nullptr);
    EXPECT_EQ(mode_sv->value().string_value(), "open_loop");

    const auto *tgt1_sv = find("motor1_target");
    ASSERT_NE(tgt1_sv, nullptr);
    EXPECT_EQ(tgt1_sv->value().int64_value(), 200);

    const auto *tgt2_sv = find("motor2_target");
    ASSERT_NE(tgt2_sv, nullptr);
    EXPECT_EQ(tgt2_sv->value().int64_value(), -150);

    // In open-loop: value1 == target1, value2 == target2
    const auto *val1_sv = find("motor1_value");
    ASSERT_NE(val1_sv, nullptr);
    EXPECT_EQ(val1_sv->value().int64_value(), 200);

    const auto *val2_sv = find("motor2_value");
    ASSERT_NE(val2_sv, nullptr);
    EXPECT_EQ(val2_sv->value().int64_value(), -150);

    const auto *brake1_sv = find("motor1_brake");
    ASSERT_NE(brake1_sv, nullptr);
    EXPECT_TRUE(brake1_sv->value().bool_value());

    const auto *brake2_sv = find("motor2_brake");
    ASSERT_NE(brake2_sv, nullptr);
    EXPECT_FALSE(brake2_sv->value().bool_value());

    const auto *estop_sv = find("estop");
    ASSERT_NE(estop_sv, nullptr);
    EXPECT_FALSE(estop_sv->value().bool_value());
}

TEST_F(DcmtAdapterTest, ReadSignals_ClosedPosition_EncoderFeedbackIsIndependent) {
    // target=1000, actual encoder reading=980, brakes off
    script_state_reply(make_closed_loop_payload(
        DCMT_MODE_CLOSED_POSITION, 1000, -1000, 980, -990, 0x00, 0x00));

    const auto result = read_signals(session, device, {});

    ASSERT_TRUE(result.ok);

    const auto find = [&](const std::string &id) -> const anolis::deviceprovider::v1::SignalValue * {
        for (const auto &sv : result.values) {
            if (sv.signal_id() == id) return &sv;
        }
        return nullptr;
    };

    EXPECT_EQ(find("mode")->value().string_value(), "closed_position");
    EXPECT_EQ(find("motor1_target")->value().int64_value(), 1000);
    EXPECT_EQ(find("motor1_value")->value().int64_value(), 980);  // independent
    EXPECT_EQ(find("motor2_target")->value().int64_value(), -1000);
    EXPECT_EQ(find("motor2_value")->value().int64_value(), -990);
}

TEST_F(DcmtAdapterTest, ReadSignals_ClosedSpeed_ModeStringCorrect) {
    script_state_reply(make_closed_loop_payload(
        DCMT_MODE_CLOSED_SPEED, 500, 500, 498, 501, 0x00, 0x00));

    const auto result = read_signals(session, device, {});

    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.values.at(0).value().string_value(), "closed_speed");
}

TEST_F(DcmtAdapterTest, ReadSignals_SubsetRequest_ReturnsOnlyRequested) {
    script_state_reply(make_open_loop_payload(100, 200, 0x00, 0x00));

    const auto result = read_signals(session, device, {"motor1_target", "mode"});

    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.values.size(), 2u);
}

TEST_F(DcmtAdapterTest, ReadSignals_Estop_DecodedCorrectly) {
    script_state_reply(make_open_loop_payload(0, 0, 0x03, 0x01));  // both brakes + estop

    const auto result = read_signals(session, device, {});
    ASSERT_TRUE(result.ok);

    for (const auto &sv : result.values) {
        if (sv.signal_id() == "estop")
            EXPECT_TRUE(sv.value().bool_value());
        if (sv.signal_id() == "motor1_brake")
            EXPECT_TRUE(sv.value().bool_value());
        if (sv.signal_id() == "motor2_brake")
            EXPECT_TRUE(sv.value().bool_value());
    }
}

TEST_F(DcmtAdapterTest, ReadSignals_TruncatedOpenLoopPayload_ReturnsInternal) {
    // Only 4 bytes — too short for open-loop (needs 7)
    transport.read_replies[DCMT_OP_GET_STATE] =
        crumbs::RawFrame{DCMT_TYPE_ID, DCMT_OP_GET_STATE, {0x00, 0x10, 0x00, 0x10}};

    const auto result = read_signals(session, device, {});

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error_code,
              anolis::deviceprovider::v1::Status::CODE_INTERNAL);
}

TEST_F(DcmtAdapterTest, ReadSignals_TruncatedClosedLoopPayload_ReturnsInternal) {
    // mode byte = closed_position but payload too short (only 6 bytes, needs 11)
    transport.read_replies[DCMT_OP_GET_STATE] =
        crumbs::RawFrame{DCMT_TYPE_ID, DCMT_OP_GET_STATE,
                         {DCMT_MODE_CLOSED_POSITION, 0x10, 0x00, 0x10, 0x00, 0x00}};

    const auto result = read_signals(session, device, {});

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error_code,
              anolis::deviceprovider::v1::Status::CODE_INTERNAL);
}

TEST_F(DcmtAdapterTest, ReadSignals_WrongTypeId_ReturnsInternal) {
    transport.read_replies[DCMT_OP_GET_STATE] =
        crumbs::RawFrame{0x99u, DCMT_OP_GET_STATE,
                         make_open_loop_payload(0, 0, 0, 0)};

    const auto result = read_signals(session, device, {});

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error_code,
              anolis::deviceprovider::v1::Status::CODE_INTERNAL);
}

TEST_F(DcmtAdapterTest, ReadSignals_SessionFails_ReturnsUnavailable) {
    transport.read_error = crumbs::SessionErrorCode::ReadFailed;

    const auto result = read_signals(session, device, {});

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error_code,
              anolis::deviceprovider::v1::Status::CODE_UNAVAILABLE);
}

// ---------------------------------------------------------------------------
// call tests
// ---------------------------------------------------------------------------

TEST_F(DcmtAdapterTest, Call_SetOpenLoop_SendsCorrectPayload) {
    const auto args = make_args({
        {"motor1_pwm", make_int64_val(200)},
        {"motor2_pwm", make_int64_val(-150)},
    });

    const auto result = call(session, device, 1u, args);

    ASSERT_TRUE(result.ok) << result.error_message;
    const auto &f = transport.sent_frames.at(0).second;
    EXPECT_EQ(f.type_id, static_cast<uint8_t>(DCMT_TYPE_ID));
    EXPECT_EQ(f.opcode, static_cast<uint8_t>(DCMT_OP_SET_OPEN_LOOP));
    ASSERT_EQ(f.payload.size(), 4u);

    int16_t m1 = 0, m2 = 0;
    read_i16_le(f.payload, 0u, m1);
    read_i16_le(f.payload, 2u, m2);
    EXPECT_EQ(m1, 200);
    EXPECT_EQ(m2, -150);
}

TEST_F(DcmtAdapterTest, Call_SetOpenLoop_PwmOverRange_ReturnsInvalidArgument) {
    const auto args = make_args({
        {"motor1_pwm", make_int64_val(40000)},  // > INT16_MAX
        {"motor2_pwm", make_int64_val(0)},
    });

    const auto result = call(session, device, 1u, args);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error_code,
              anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT);
}

TEST_F(DcmtAdapterTest, Call_SetBrake_SendsCorrectPayload) {
    const auto args = make_args({
        {"motor1_brake", make_bool_val(true)},
        {"motor2_brake", make_bool_val(false)},
    });

    const auto result = call(session, device, 2u, args);

    ASSERT_TRUE(result.ok);
    const auto &payload = transport.sent_frames.at(0).second.payload;
    ASSERT_EQ(payload.size(), 2u);
    EXPECT_EQ(payload.at(0), 1u);
    EXPECT_EQ(payload.at(1), 0u);
}

TEST_F(DcmtAdapterTest, Call_SetMode_OpenLoop_SendsCorrectByte) {
    const auto args = make_args({{"mode", make_string_val("open_loop")}});

    const auto result = call(session, device, 3u, args);

    ASSERT_TRUE(result.ok);
    ASSERT_EQ(transport.sent_frames.at(0).second.payload.size(), 1u);
    EXPECT_EQ(transport.sent_frames.at(0).second.payload.at(0),
              static_cast<uint8_t>(DCMT_MODE_OPEN_LOOP));
}

TEST_F(DcmtAdapterTest, Call_SetMode_ClosedPosition_SendsCorrectByte) {
    const auto args = make_args({{"mode", make_string_val("closed_position")}});

    ASSERT_TRUE(call(session, device, 3u, args).ok);
    EXPECT_EQ(transport.sent_frames.at(0).second.payload.at(0),
              static_cast<uint8_t>(DCMT_MODE_CLOSED_POSITION));
}

TEST_F(DcmtAdapterTest, Call_SetMode_InvalidMode_ReturnsInvalidArgument) {
    const auto args = make_args({{"mode", make_string_val("warp")}});

    const auto result = call(session, device, 3u, args);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error_code,
              anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT);
}

TEST_F(DcmtAdapterTest, Call_SetSetpoint_SendsCorrectPayload) {
    const auto args = make_args({
        {"motor1_target", make_int64_val(1000)},
        {"motor2_target", make_int64_val(-500)},
    });

    const auto result = call(session, device, 4u, args);

    ASSERT_TRUE(result.ok);
    const auto &payload = transport.sent_frames.at(0).second.payload;
    ASSERT_EQ(payload.size(), 4u);

    int16_t t1 = 0, t2 = 0;
    read_i16_le(payload, 0u, t1);
    read_i16_le(payload, 2u, t2);
    EXPECT_EQ(t1, 1000);
    EXPECT_EQ(t2, -500);
}

TEST_F(DcmtAdapterTest, Call_SetPidX10_ValidArgs_SendsSixBytes) {
    const auto args = make_args({
        {"kp1_x10", make_uint64_val(20u)},
        {"ki1_x10", make_uint64_val(10u)},
        {"kd1_x10", make_uint64_val(5u)},
        {"kp2_x10", make_uint64_val(18u)},
        {"ki2_x10", make_uint64_val(8u)},
        {"kd2_x10", make_uint64_val(4u)},
    });

    const auto result = call(session, device, 5u, args);

    ASSERT_TRUE(result.ok);
    ASSERT_EQ(transport.sent_frames.at(0).second.payload.size(), 6u);
    EXPECT_EQ(transport.sent_frames.at(0).second.payload.at(0), 20u);
}

TEST_F(DcmtAdapterTest, Call_UnknownFunctionId_ReturnsNotFound) {
    const auto result = call(session, device, 99u, make_args({}));

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error_code,
              anolis::deviceprovider::v1::Status::CODE_NOT_FOUND);
}

TEST_F(DcmtAdapterTest, Call_MissingRequiredArg_ReturnsInvalidArgument) {
    // set_open_loop: only motor1_pwm provided
    const auto args = make_args({{"motor1_pwm", make_int64_val(100)}});

    const auto result = call(session, device, 1u, args);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error_code,
              anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT);
}

} // namespace
} // namespace anolis_provider_bread::dcmt
