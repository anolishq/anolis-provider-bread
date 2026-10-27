#include "devices/common/watchdog_keepalive.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "crumbs/crumbs_canned_bus.hpp"
#include "crumbs/crumbs_transport.hpp"
#include "crumbs/session.hpp"
#include "devices/common/inventory.hpp"

extern "C" {
#include <bread/bread_watchdog.h>
#include <bread/dcmt_ops.h>
#include <bread/rlht_ops.h>
}

// #190: from arming until the runtime's first call, nothing else reaches the
// boards, so the keepalive must feed each armed watchdog well inside its
// timeout, stop at that device's first call, and give up at the cap.

namespace anolis_provider_bread::watchdog {
namespace {

using Clock = Keepalive::Clock;
using std::chrono::milliseconds;

// Forwards to a CrumbsTransport over a canned bus and records each frame sent,
// by address.
class RecordingTransport final : public crumbs::Transport {
public:
    explicit RecordingTransport(std::unique_ptr<crumbs::CrumbsCannedBus> bus) : inner_(std::move(bus)) {}

    crumbs::SessionStatus open(const crumbs::SessionOptions &options) override { return inner_.open(options); }
    void close() noexcept override { inner_.close(); }
    bool is_open() const override { return inner_.is_open(); }
    crumbs::SessionStatus scan(const crumbs::ScanOptions &options, std::vector<crumbs::ScanResult> &out) override {
        return inner_.scan(options, out);
    }
    crumbs::SessionStatus send(uint8_t address, const crumbs::RawFrame &frame) override {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            sent_[address].push_back(frame);
        }
        return inner_.send(address, frame);
    }
    crumbs::SessionStatus read(uint8_t address, crumbs::RawFrame &frame, uint32_t timeout_us) override {
        return inner_.read(address, frame, timeout_us);
    }
    void delay_us(uint32_t delay) override { inner_.delay_us(delay); }

    std::vector<crumbs::RawFrame> sent_to(uint8_t address) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sent_.find(address);
        return it == sent_.end() ? std::vector<crumbs::RawFrame>{} : it->second;
    }

private:
    crumbs::CrumbsTransport inner_;
    mutable std::mutex mutex_;
    std::map<uint8_t, std::vector<crumbs::RawFrame>> sent_;
};

inventory::InventoryDevice make_device(const std::string &id, DeviceType type, uint8_t address, uint32_t caps,
                                       int command_watchdog_ms) {
    inventory::InventoryDevice device;
    device.descriptor.set_device_id(id);
    device.type = type;
    device.address = address;
    device.capability_profile.flags = caps;
    device.command_watchdog_ms = command_watchdog_ms;
    return device;
}

class ClogCapture {
public:
    ClogCapture() : saved_(std::clog.rdbuf(captured_.rdbuf())) {}
    ~ClogCapture() { std::clog.rdbuf(saved_); }
    ClogCapture(const ClogCapture &) = delete;
    ClogCapture &operator=(const ClogCapture &) = delete;
    std::string text() const { return captured_.str(); }

private:
    std::ostringstream captured_;
    std::streambuf *saved_;
};

class KeepaliveTest : public ::testing::Test {
protected:
    static constexpr uint8_t kDcmt = 0x14;
    static constexpr uint8_t kRlht = 0x08;
    static constexpr uint8_t kUnarmed = 0x15;
    static constexpr uint8_t kNoCapability = 0x16;

    KeepaliveTest() {
        auto canned = std::make_unique<crumbs::CrumbsCannedBus>("mock://keepalive-test");
        canned->add_device(kDcmt, DCMT_TYPE_ID);
        canned->add_device(kRlht, RLHT_TYPE_ID);
        canned->add_device(kUnarmed, DCMT_TYPE_ID);
        canned->add_device(kNoCapability, DCMT_TYPE_ID);
        transport_ = std::make_unique<RecordingTransport>(std::move(canned));
        session_ =
            std::make_unique<crumbs::Session>(*transport_, crumbs::SessionOptions{"mock://keepalive-test", 0u, 50u, 0});
        EXPECT_TRUE(session_->open());
        devices_ = {
            make_device("dcmt0", DeviceType::Dcmt, kDcmt, DCMT_CAP_CMD_WATCHDOG, 400),
            make_device("rlht0", DeviceType::Rlht, kRlht, RLHT_CAP_CMD_WATCHDOG, 1000),
            make_device("dcmt1", DeviceType::Dcmt, kUnarmed, DCMT_CAP_CMD_WATCHDOG, 0),
            make_device("dcmt2", DeviceType::Dcmt, kNoCapability, DCMT_CAP_BASELINE_FLAGS, 400),
        };
    }

    std::unique_ptr<RecordingTransport> transport_;
    std::unique_ptr<crumbs::Session> session_;
    std::vector<inventory::InventoryDevice> devices_;
};

TEST_F(KeepaliveTest, FeedsEachArmedWatchdogWithinAQuarterOfItsTimeoutUntilItsFirstCall) {
    const Clock::time_point armed = Clock::time_point{} + std::chrono::hours{1};
    Keepalive keepalive(*session_, devices_, milliseconds{30000}, armed);
    constexpr milliseconds kStep{5};

    // A provider-side clock stepped through 2 s: record when each device was fed.
    std::map<uint8_t, std::vector<Clock::time_point>> fed_at;
    std::map<uint8_t, size_t> seen;
    for (Clock::time_point now = armed; now <= armed + milliseconds{2000}; now += kStep) {
        if (now == armed + milliseconds{1000}) {
            keepalive.contacted("dcmt0", now);
        }
        keepalive.tick(now);
        for (const uint8_t address : {kDcmt, kRlht, kUnarmed, kNoCapability}) {
            const size_t count = transport_->sent_to(address).size();
            if (count != seen[address]) {
                fed_at[address].push_back(now);
                seen[address] = count;
            }
        }
    }

    // Each feed is a GET_WATCHDOG query, and the longest unfed stretch,
    // counted from arming, is a quarter of the timeout (plus one clock step).
    const auto longest_gap = [&](uint8_t address, Clock::time_point until) {
        Clock::time_point last = armed;
        Clock::duration longest{};
        for (const Clock::time_point at : fed_at[address]) {
            longest = std::max(longest, at - last);
            last = at;
        }
        return std::max(longest, until - last);
    };
    for (const auto &frame : transport_->sent_to(kDcmt)) {
        ASSERT_FALSE(frame.payload.empty());
        EXPECT_EQ(frame.payload[0], BREAD_OP_GET_WATCHDOG);
    }
    EXPECT_LE(longest_gap(kDcmt, armed + milliseconds{1000}), milliseconds{100} + kStep);
    EXPECT_LE(longest_gap(kRlht, armed + milliseconds{2000}), milliseconds{250} + kStep);

    // dcmt0's first call ends its feeding; rlht0 is still fed.
    ASSERT_FALSE(fed_at[kDcmt].empty());
    EXPECT_LE(fed_at[kDcmt].back(), armed + milliseconds{1000});
    EXPECT_GT(fed_at[kRlht].back(), armed + milliseconds{1750});

    // Nothing is sent to a device with no watchdog, or firmware without one.
    EXPECT_TRUE(fed_at[kUnarmed].empty());
    EXPECT_TRUE(fed_at[kNoCapability].empty());
}

TEST_F(KeepaliveTest, FirstCallLogsTheMarginOnce) {
    const Clock::time_point armed = Clock::time_point{} + std::chrono::hours{1};
    Keepalive keepalive(*session_, devices_, milliseconds{30000}, armed);
    std::string log;
    {
        const ClogCapture capture;
        keepalive.contacted("dcmt0", armed + milliseconds{4800});
        keepalive.contacted("dcmt0", armed + milliseconds{4900});
        log = capture.text();
    }
    const std::string line = "watchdog: dcmt0 (0x14) first call 4800 ms after arming; fed every 100 ms until then";
    ASSERT_NE(log.find(line), std::string::npos) << log;
    EXPECT_EQ(log.find(line, log.find(line) + 1), std::string::npos) << log;
}

TEST_F(KeepaliveTest, StopsFeedingAtTheCapAndSaysWhich) {
    const Clock::time_point armed = Clock::time_point{} + std::chrono::hours{1};
    Keepalive keepalive(*session_, devices_, milliseconds{300}, armed);
    keepalive.contacted("dcmt0", armed + milliseconds{50});

    std::string log;
    {
        const ClogCapture capture;
        for (Clock::time_point now = armed; now < armed + milliseconds{300}; now += milliseconds{10}) {
            EXPECT_TRUE(keepalive.tick(now).has_value());
        }
        const size_t before = transport_->sent_to(kRlht).size();
        EXPECT_FALSE(keepalive.tick(armed + milliseconds{300}).has_value());
        EXPECT_FALSE(keepalive.tick(armed + milliseconds{2000}).has_value());
        EXPECT_EQ(transport_->sent_to(kRlht).size(), before);
        log = capture.text();
    }
    EXPECT_NE(log.find("[WARN] watchdog: no call from the runtime for rlht0 (0x08) within 300 ms of arming; no "
                       "longer feeding its watchdog"),
              std::string::npos)
        << log;
    EXPECT_EQ(log.find("for dcmt0"), std::string::npos) << log;
}

TEST_F(KeepaliveTest, ItsThreadFeedsUntilDestroyed) {
    std::vector<inventory::InventoryDevice> fast = {
        make_device("dcmt0", DeviceType::Dcmt, kDcmt, DCMT_CAP_CMD_WATCHDOG, 40),
    };
    {
        Keepalive keepalive(*session_, fast);
        keepalive.start();
        std::this_thread::sleep_for(milliseconds{200});
    }
    const size_t fed = transport_->sent_to(kDcmt).size();
    EXPECT_GE(fed, 2u);
    std::this_thread::sleep_for(milliseconds{50});
    EXPECT_EQ(transport_->sent_to(kDcmt).size(), fed) << "the destructor stops the thread";
}

}  // namespace
}  // namespace anolis_provider_bread::watchdog
