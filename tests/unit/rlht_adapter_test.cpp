#include "devices/rlht/rlht_adapter.hpp"

#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "crumbs/session.hpp"
#include "devices/common/inventory.hpp"

extern "C" {
#include <bread/rlht_ops.h>
}

namespace anolis_provider_bread::rlht {
namespace {

// ---------------------------------------------------------------------------
// Minimal scripted transport for adapter tests.
//
// For read_signals (query_read): script a reply by opcode via read_replies.
// For call (session.send): sent frames are recorded in sent_frames.
// ---------------------------------------------------------------------------

class AdapterTestTransport final : public crumbs::Transport {
public:
  // Records all frames passed to send().
  std::vector<std::pair<uint8_t, crumbs::RawFrame>> sent_frames;

  // Script a reply returned by read() for a given pending opcode.
  std::map<uint8_t, crumbs::RawFrame> read_replies;

  // Set to non-Ok to make the next read() fail.
  crumbs::SessionErrorCode read_error = crumbs::SessionErrorCode::Ok;
  // Set to non-Ok to make the next send() fail.
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
      return crumbs::SessionStatus::failure(
          crumbs::SessionErrorCode::ReadFailed, "no scripted reply for opcode");
    }
    out = it->second;
    return crumbs::SessionStatus::success();
  }

private:
  bool open_ = false;
  uint8_t pending_opcode_ = 0;
};

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

struct RlhtAdapterTest : public ::testing::Test {
  AdapterTestTransport transport;
  crumbs::Session session{transport, [] {
                            crumbs::SessionOptions opts;
                            opts.bus_path = "/dev/i2c-test";
                            return opts;
                          }()};

  inventory::InventoryDevice device;

  void SetUp() override {
    ASSERT_TRUE(session.open().ok());
    device.address = 0x08;
    device.type = DeviceType::Rlht;
  }

  // Build a 19-byte RLHT GET_STATE payload.
  static std::vector<uint8_t>
  make_state_payload(uint8_t mode, uint8_t flags, int16_t t1_dc, int16_t t2_dc,
                     int16_t sp1_dc, int16_t sp2_dc, uint16_t on1_ms,
                     uint16_t on2_ms, uint16_t period1_ms, uint16_t period2_ms,
                     uint8_t tc_select) {
    std::vector<uint8_t> p;
    append_u8(p, mode);
    append_u8(p, flags);
    append_i16_le(p, t1_dc);
    append_i16_le(p, t2_dc);
    append_i16_le(p, sp1_dc);
    append_i16_le(p, sp2_dc);
    append_u16_le(p, on1_ms);
    append_u16_le(p, on2_ms);
    append_u16_le(p, period1_ms);
    append_u16_le(p, period2_ms);
    append_u8(p, tc_select);
    return p;
  }

  void script_state_reply(std::vector<uint8_t> payload) {
    transport.read_replies[RLHT_OP_GET_STATE] =
        crumbs::RawFrame{RLHT_TYPE_ID, RLHT_OP_GET_STATE, std::move(payload)};
  }

  // Build a protobuf Value map from initializer list of {key, Value}.
  static ValueMap
  make_args(std::initializer_list<
            std::pair<std::string, anolis::deviceprovider::v1::Value>>
                pairs) {
    ValueMap m;
    for (const auto &kv : pairs) {
      m[kv.first] = kv.second;
    }
    return m;
  }
};

// ---------------------------------------------------------------------------
// read_signals tests
// ---------------------------------------------------------------------------

TEST_F(RlhtAdapterTest, ReadSignals_ClosedLoopState_ReturnsAllSignals) {
  // 250.5°C, 251.0°C, setpoints 200.0/201.0, period 5000/6000 ms
  // flags: relay1 on, estop off
  script_state_reply(
      make_state_payload(RLHT_MODE_CLOSED_LOOP, RLHT_FLAG_RELAY1_ON, 2505,
                         2510,       // t1=250.5°C, t2=251.0°C (deci-C)
                         2000, 2010, // sp1=200.0°C, sp2=201.0°C
                         500, 600,   // on1_ms, on2_ms (not exposed)
                         5000, 6000, // period1_ms, period2_ms
                         0x00));

  const auto result = read_signals(session, device, {});

  ASSERT_TRUE(result.ok) << result.error_message;
  ASSERT_EQ(result.values.size(), 10u);

  const auto find = [&](const std::string &id)
      -> const anolis::deviceprovider::v1::SignalValue * {
    for (const auto &sv : result.values) {
      if (sv.signal_id() == id)
        return &sv;
    }
    return nullptr;
  };

  const auto *mode_sv = find("mode");
  ASSERT_NE(mode_sv, nullptr);
  EXPECT_EQ(mode_sv->value().string_value(), "closed_loop");

  const auto *t1_sv = find("t1_c");
  ASSERT_NE(t1_sv, nullptr);
  EXPECT_NEAR(t1_sv->value().double_value(), 250.5, 0.01);

  const auto *t2_sv = find("t2_c");
  ASSERT_NE(t2_sv, nullptr);
  EXPECT_NEAR(t2_sv->value().double_value(), 251.0, 0.01);

  const auto *sp1_sv = find("setpoint1_c");
  ASSERT_NE(sp1_sv, nullptr);
  EXPECT_NEAR(sp1_sv->value().double_value(), 200.0, 0.01);

  const auto *period1_sv = find("period1_ms");
  ASSERT_NE(period1_sv, nullptr);
  EXPECT_EQ(period1_sv->value().uint64_value(), 5000u);

  const auto *period2_sv = find("period2_ms");
  ASSERT_NE(period2_sv, nullptr);
  EXPECT_EQ(period2_sv->value().uint64_value(), 6000u);

  const auto *relay1_sv = find("relay1_on");
  ASSERT_NE(relay1_sv, nullptr);
  EXPECT_TRUE(relay1_sv->value().bool_value());

  const auto *relay2_sv = find("relay2_on");
  ASSERT_NE(relay2_sv, nullptr);
  EXPECT_FALSE(relay2_sv->value().bool_value());

  const auto *estop_sv = find("estop");
  ASSERT_NE(estop_sv, nullptr);
  EXPECT_FALSE(estop_sv->value().bool_value());
}

TEST_F(RlhtAdapterTest, ReadSignals_OpenLoopState_ReturnsOpenLoopMode) {
  script_state_reply(make_state_payload(RLHT_MODE_OPEN_LOOP, RLHT_FLAG_ESTOP, 0,
                                        0, 0, 0, 0, 0, 1000, 1000, 0x00));

  const auto result = read_signals(session, device, {});

  ASSERT_TRUE(result.ok);
  const auto &mode_sv = result.values.at(0);
  EXPECT_EQ(mode_sv.value().string_value(), "open_loop");

  // estop flag should be set
  for (const auto &sv : result.values) {
    if (sv.signal_id() == "estop") {
      EXPECT_TRUE(sv.value().bool_value());
    }
  }
}

TEST_F(RlhtAdapterTest, ReadSignals_SubsetRequest_ReturnsOnlyRequestedSignals) {
  script_state_reply(make_state_payload(RLHT_MODE_CLOSED_LOOP, 0x00, 1000, 1010,
                                        950, 960, 0, 0, 2000, 2000, 0x00));

  const auto result = read_signals(session, device, {"t1_c", "t2_c"});

  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.values.size(), 2u);
  EXPECT_EQ(result.values.at(0).signal_id(), "t1_c");
  EXPECT_EQ(result.values.at(1).signal_id(), "t2_c");
  EXPECT_NEAR(result.values.at(0).value().double_value(), 100.0, 0.01);
  EXPECT_NEAR(result.values.at(1).value().double_value(), 101.0, 0.01);
}

TEST_F(RlhtAdapterTest, ReadSignals_NegativeTemperature_ParsedCorrectly) {
  // -45.3°C = -453 deci-C
  script_state_reply(make_state_payload(RLHT_MODE_CLOSED_LOOP, 0x00,
                                        static_cast<int16_t>(-453), 0, 0, 0, 0,
                                        0, 1000, 1000, 0x00));

  const auto result = read_signals(session, device, {"t1_c"});

  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.values.size(), 1u);
  EXPECT_NEAR(result.values.at(0).value().double_value(), -45.3, 0.01);
}

TEST_F(RlhtAdapterTest, ReadSignals_SessionReadFails_ReturnsUnavailable) {
  transport.read_error = crumbs::SessionErrorCode::ReadFailed;

  const auto result = read_signals(session, device, {});

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error_code,
            anolis::deviceprovider::v1::Status::CODE_UNAVAILABLE);
}

TEST_F(RlhtAdapterTest, ReadSignals_SessionTimesOut_ReturnsDeadlineExceeded) {
  transport.read_error = crumbs::SessionErrorCode::Timeout;

  const auto result = read_signals(session, device, {});

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error_code,
            anolis::deviceprovider::v1::Status::CODE_DEADLINE_EXCEEDED);
}

TEST_F(RlhtAdapterTest, ReadSignals_TruncatedPayload_ReturnsInternal) {
  // Only 5 bytes — far too short for a valid RLHT state frame
  transport.read_replies[RLHT_OP_GET_STATE] = crumbs::RawFrame{
      RLHT_TYPE_ID, RLHT_OP_GET_STATE, {0x00, 0x00, 0x00, 0x00, 0x00}};

  const auto result = read_signals(session, device, {});

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error_code,
            anolis::deviceprovider::v1::Status::CODE_INTERNAL);
}

TEST_F(RlhtAdapterTest, ReadSignals_WrongTypeId_ReturnsInternal) {
  transport.read_replies[RLHT_OP_GET_STATE] =
      crumbs::RawFrame{0x99u, RLHT_OP_GET_STATE,
                       make_state_payload(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)};

  const auto result = read_signals(session, device, {});

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error_code,
            anolis::deviceprovider::v1::Status::CODE_INTERNAL);
}

// ---------------------------------------------------------------------------
// call tests
// ---------------------------------------------------------------------------

TEST_F(RlhtAdapterTest, Call_SetMode_ClosedLoop_SendsCorrectFrame) {
  const auto args = make_args({{"mode", make_string_val("closed_loop")}});

  const auto result = call(session, device, 1u, args);

  ASSERT_TRUE(result.ok) << result.error_message;
  ASSERT_EQ(transport.sent_frames.size(), 1u);
  const auto &f = transport.sent_frames.at(0).second;
  EXPECT_EQ(f.type_id, static_cast<uint8_t>(RLHT_TYPE_ID));
  EXPECT_EQ(f.opcode, static_cast<uint8_t>(RLHT_OP_SET_MODE));
  ASSERT_EQ(f.payload.size(), 1u);
  EXPECT_EQ(f.payload.at(0), static_cast<uint8_t>(RLHT_MODE_CLOSED_LOOP));
}

TEST_F(RlhtAdapterTest, Call_SetMode_OpenLoop_SendsCorrectByte) {
  const auto args = make_args({{"mode", make_string_val("open_loop")}});

  const auto result = call(session, device, 1u, args);

  ASSERT_TRUE(result.ok);
  EXPECT_EQ(transport.sent_frames.at(0).second.payload.at(0),
            static_cast<uint8_t>(RLHT_MODE_OPEN_LOOP));
}

TEST_F(RlhtAdapterTest, Call_SetMode_InvalidMode_ReturnsInvalidArgument) {
  const auto args = make_args({{"mode", make_string_val("turbo")}});

  const auto result = call(session, device, 1u, args);

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error_code,
            anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT);
}

TEST_F(RlhtAdapterTest, Call_SetSetpoints_SendsCorrectPayload) {
  // 185.5°C and 190.0°C → 1855 and 1900 deci-C
  const auto args = make_args({
      {"setpoint1_c", make_double_val(185.5)},
      {"setpoint2_c", make_double_val(190.0)},
  });

  const auto result = call(session, device, 2u, args);

  ASSERT_TRUE(result.ok) << result.error_message;
  const auto &payload = transport.sent_frames.at(0).second.payload;
  ASSERT_EQ(payload.size(), 4u);
  EXPECT_EQ(transport.sent_frames.at(0).second.opcode,
            static_cast<uint8_t>(RLHT_OP_SET_SETPOINTS));

  // Decode i16 little-endian
  int16_t sp1 = 0, sp2 = 0;
  read_i16_le(payload, 0u, sp1);
  read_i16_le(payload, 2u, sp2);
  EXPECT_EQ(sp1, 1855);
  EXPECT_EQ(sp2, 1900);
}

TEST_F(RlhtAdapterTest, Call_SetSetpoints_OutOfRange_ReturnsInvalidArgument) {
  const auto args = make_args({
      {"setpoint1_c", make_double_val(4000.0)},
      {"setpoint2_c", make_double_val(25.0)},
  });

  const auto result = call(session, device, 2u, args);

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error_code,
            anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT);
  EXPECT_EQ(result.error_message,
            "setpoint values must be in [-3276.8, 3276.7] C");
}

TEST_F(RlhtAdapterTest, Call_SetPidX10_ValidArgs_SendsSixBytes) {
  const auto args = make_args({
      {"kp1_x10", make_uint64_val(10u)},
      {"ki1_x10", make_uint64_val(5u)},
      {"kd1_x10", make_uint64_val(2u)},
      {"kp2_x10", make_uint64_val(12u)},
      {"ki2_x10", make_uint64_val(6u)},
      {"kd2_x10", make_uint64_val(3u)},
  });

  const auto result = call(session, device, 3u, args);

  ASSERT_TRUE(result.ok);
  const auto &payload = transport.sent_frames.at(0).second.payload;
  ASSERT_EQ(payload.size(), 6u);
  EXPECT_EQ(payload.at(0), 10u);
  EXPECT_EQ(payload.at(3), 12u);
}

TEST_F(RlhtAdapterTest, Call_SetPidX10_ValueTooLarge_ReturnsInvalidArgument) {
  const auto args = make_args({
      {"kp1_x10", make_uint64_val(256u)}, // > 255
      {"ki1_x10", make_uint64_val(0u)},
      {"kd1_x10", make_uint64_val(0u)},
      {"kp2_x10", make_uint64_val(0u)},
      {"ki2_x10", make_uint64_val(0u)},
      {"kd2_x10", make_uint64_val(0u)},
  });

  const auto result = call(session, device, 3u, args);

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error_code,
            anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT);
}

TEST_F(RlhtAdapterTest, Call_SetPeriodsMs_SendsFourBytes) {
  const auto args = make_args({
      {"period1_ms", make_uint64_val(5000u)},
      {"period2_ms", make_uint64_val(6000u)},
  });

  const auto result = call(session, device, 4u, args);

  ASSERT_TRUE(result.ok);
  const auto &payload = transport.sent_frames.at(0).second.payload;
  ASSERT_EQ(payload.size(), 4u);
  uint16_t p1 = 0, p2 = 0;
  read_u16_le(payload, 0u, p1);
  read_u16_le(payload, 2u, p2);
  EXPECT_EQ(p1, 5000u);
  EXPECT_EQ(p2, 6000u);
}

TEST_F(RlhtAdapterTest, Call_SetOpenDutyPct_ValidRange_SendsTwoBytes) {
  const auto args = make_args({
      {"duty1_pct", make_uint64_val(75u)},
      {"duty2_pct", make_uint64_val(50u)},
  });

  const auto result = call(session, device, 6u, args);

  ASSERT_TRUE(result.ok);
  const auto &payload = transport.sent_frames.at(0).second.payload;
  ASSERT_EQ(payload.size(), 2u);
  EXPECT_EQ(payload.at(0), 75u);
  EXPECT_EQ(payload.at(1), 50u);
}

TEST_F(RlhtAdapterTest, Call_SetOpenDutyPct_Over100_ReturnsInvalidArgument) {
  const auto args = make_args({
      {"duty1_pct", make_uint64_val(101u)},
      {"duty2_pct", make_uint64_val(50u)},
  });

  const auto result = call(session, device, 6u, args);

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error_code,
            anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT);
}

TEST_F(RlhtAdapterTest, Call_MissingRequiredArg_ReturnsInvalidArgument) {
  // set_setpoints: only provide one of the two required args
  const auto args = make_args({
      {"setpoint1_c", make_double_val(100.0)},
      // setpoint2_c is missing
  });

  const auto result = call(session, device, 2u, args);

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error_code,
            anolis::deviceprovider::v1::Status::CODE_INVALID_ARGUMENT);
}

TEST_F(RlhtAdapterTest, Call_UnknownFunctionId_ReturnsNotFound) {
  const auto args = make_args({});

  const auto result = call(session, device, 99u, args);

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error_code,
            anolis::deviceprovider::v1::Status::CODE_NOT_FOUND);
}

TEST_F(RlhtAdapterTest, Call_SendFails_ReturnsUnavailable) {
  transport.send_error = crumbs::SessionErrorCode::WriteFailed;

  const auto args = make_args({{"mode", make_string_val("closed_loop")}});
  const auto result = call(session, device, 1u, args);

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error_code,
            anolis::deviceprovider::v1::Status::CODE_UNAVAILABLE);
}

TEST_F(RlhtAdapterTest, Call_SendTimesOut_ReturnsDeadlineExceeded) {
  transport.send_error = crumbs::SessionErrorCode::Timeout;

  const auto args = make_args({{"mode", make_string_val("closed_loop")}});
  const auto result = call(session, device, 1u, args);

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error_code,
            anolis::deviceprovider::v1::Status::CODE_DEADLINE_EXCEEDED);
}

} // namespace
} // namespace anolis_provider_bread::rlht
