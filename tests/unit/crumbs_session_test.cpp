#include "crumbs/session.hpp"

#include <deque>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace anolis_provider_bread::crumbs {
namespace {

class FakeTransport final : public Transport {
public:
    struct ReadAction {
        SessionStatus status = SessionStatus::success();
        RawFrame frame;
    };

    SessionStatus open(const SessionOptions &options) override {
        ++open_calls;
        last_open_options = options;
        if(!open_result) {
            open_state = false;
            return open_result;
        }
        open_state = true;
        return open_result;
    }

    void close() noexcept override {
        ++close_calls;
        open_state = false;
    }

    bool is_open() const override {
        return open_state;
    }

    SessionStatus scan(const ScanOptions &, std::vector<ScanResult> &out) override {
        ++scan_calls;
        if(scan_actions.empty()) {
            out = scan_results;
            return SessionStatus::success();
        }

        const SessionStatus status = scan_actions.front();
        scan_actions.pop_front();
        if(status) {
            out = scan_results;
        }
        return status;
    }

    SessionStatus send(uint8_t address, const RawFrame &frame) override {
        ++send_calls;
        sent_addresses.push_back(address);
        sent_frames.push_back(frame);
        if(send_actions.empty()) {
            return SessionStatus::success();
        }

        const SessionStatus status = send_actions.front();
        send_actions.pop_front();
        return status;
    }

    SessionStatus read(uint8_t address, RawFrame &frame, uint32_t timeout_us) override {
        ++read_calls;
        read_addresses.push_back(address);
        read_timeouts.push_back(timeout_us);
        if(read_actions.empty()) {
            return SessionStatus::failure(
                SessionErrorCode::ReadFailed,
                "no fake read action configured");
        }

        ReadAction action = read_actions.front();
        read_actions.pop_front();
        if(action.status) {
            frame = action.frame;
        }
        return action.status;
    }

    void delay_us(uint32_t delay_us) override {
        delays.push_back(delay_us);
    }

    SessionStatus open_result = SessionStatus::success();
    bool open_state = false;
    int open_calls = 0;
    int close_calls = 0;
    int scan_calls = 0;
    int send_calls = 0;
    int read_calls = 0;
    SessionOptions last_open_options;
    std::deque<SessionStatus> scan_actions;
    std::deque<SessionStatus> send_actions;
    std::deque<ReadAction> read_actions;
    std::vector<ScanResult> scan_results;
    std::vector<uint8_t> sent_addresses;
    std::vector<RawFrame> sent_frames;
    std::vector<uint8_t> read_addresses;
    std::vector<uint32_t> read_timeouts;
    std::vector<uint32_t> delays;
};

TEST(CrumbsSessionTest, BuildsSessionOptionsFromProviderConfig) {
    ProviderConfig config;
    config.bus_path = "/dev/i2c-1";
    config.query_delay_us = 12000;
    config.timeout_ms = 250;
    config.retry_count = 4;

    const SessionOptions options = make_session_options(config);
    EXPECT_EQ(options.bus_path, "/dev/i2c-1");
    EXPECT_EQ(options.query_delay_us, 12000U);
    EXPECT_EQ(options.timeout_ms, 250U);
    EXPECT_EQ(options.retry_count, 4);
}

TEST(CrumbsSessionTest, OpensTransportWithConfiguredOptions) {
    FakeTransport transport;
    Session session(transport, SessionOptions{"/dev/i2c-1", 10000u, 100u, 2});

    const SessionStatus status = session.open();
    ASSERT_TRUE(status);
    EXPECT_TRUE(session.is_open());
    EXPECT_EQ(transport.open_calls, 1);
    EXPECT_EQ(transport.last_open_options.bus_path, "/dev/i2c-1");
}

TEST(CrumbsSessionTest, QueryReadRetriesWholeOperationOnRetryableFailure) {
    FakeTransport transport;
    Session session(transport, SessionOptions{"/dev/i2c-1", 15000u, 100u, 2});
    ASSERT_TRUE(session.open());

    transport.send_actions.push_back(SessionStatus::failure(
        SessionErrorCode::WriteFailed,
        "temporary NACK"));
    transport.send_actions.push_back(SessionStatus::success());
    transport.read_actions.push_back({SessionStatus::success(), RawFrame{0x01, 0x80, {0x11, 0x22}}});

    RawFrame reply;
    const SessionStatus status = session.query_read(0x08u, 0x80u, reply);
    ASSERT_TRUE(status);
    EXPECT_EQ(status.attempts, 2);
    ASSERT_EQ(transport.send_calls, 2);
    ASSERT_EQ(transport.read_calls, 1);
    ASSERT_EQ(transport.delays.size(), 1U);
    EXPECT_EQ(transport.delays[0], 15000U);
    ASSERT_EQ(transport.sent_frames.size(), 2U);
    EXPECT_EQ(transport.sent_frames[0].type_id, 0U);
    EXPECT_EQ(transport.sent_frames[0].opcode, kSetReplyOpcode);
    ASSERT_EQ(transport.sent_frames[0].payload.size(), 1U);
    EXPECT_EQ(transport.sent_frames[0].payload[0], 0x80U);
    EXPECT_EQ(reply.type_id, 0x01U);
    EXPECT_EQ(reply.opcode, 0x80U);
    ASSERT_EQ(reply.payload.size(), 2U);
    EXPECT_EQ(reply.payload[0], 0x11U);
}

TEST(CrumbsSessionTest, QueryReadStopsOnNonRetryableFailure) {
    FakeTransport transport;
    Session session(transport, SessionOptions{"/dev/i2c-1", 10000u, 100u, 3});
    ASSERT_TRUE(session.open());

    transport.send_actions.push_back(SessionStatus::failure(
        SessionErrorCode::InvalidArgument,
        "bad frame"));

    RawFrame reply;
    const SessionStatus status = session.query_read(0x08u, 0x80u, reply);
    EXPECT_FALSE(status);
    EXPECT_EQ(status.code, SessionErrorCode::InvalidArgument);
    EXPECT_EQ(status.attempts, 1);
    EXPECT_EQ(transport.send_calls, 1);
    EXPECT_TRUE(transport.delays.empty());
    EXPECT_EQ(transport.read_calls, 0);
}

TEST(CrumbsSessionTest, ScanRequiresOpenAndReturnsResults) {
    FakeTransport transport;
    Session session(transport, SessionOptions{"/dev/i2c-1", 10000u, 100u, 1});

    std::vector<ScanResult> results;
    SessionStatus status = session.scan(ScanOptions{}, results);
    EXPECT_FALSE(status);
    EXPECT_EQ(status.code, SessionErrorCode::NotOpen);

    ASSERT_TRUE(session.open());
    transport.scan_results = {
        ScanResult{0x08u, true, 0x01u},
        ScanResult{0x09u, true, 0x02u},
    };

    status = session.scan(ScanOptions{}, results);
    ASSERT_TRUE(status);
    ASSERT_EQ(results.size(), 2U);
    EXPECT_EQ(results[0].address, 0x08U);
    EXPECT_EQ(results[0].type_id, 0x01U);
    EXPECT_EQ(results[1].address, 0x09U);
    EXPECT_EQ(results[1].type_id, 0x02U);
}

TEST(CrumbsSessionTest, RejectsOversizedPayloadBeforeTransportCall) {
    FakeTransport transport;
    Session session(transport, SessionOptions{"/dev/i2c-1", 10000u, 100u, 1});
    ASSERT_TRUE(session.open());

    RawFrame frame;
    frame.type_id = 0x01u;
    frame.opcode = 0x02u;
    frame.payload.resize(kMaxPayloadBytes + 1u, 0xAAu);

    const SessionStatus status = session.send(0x08u, frame);
    EXPECT_FALSE(status);
    EXPECT_EQ(status.code, SessionErrorCode::InvalidArgument);
    EXPECT_EQ(transport.send_calls, 0);
}

} // namespace
} // namespace anolis_provider_bread::crumbs