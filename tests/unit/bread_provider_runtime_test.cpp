#include "core/bread_provider_runtime.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "anolis/provider_sdk/host_check.hpp"
#include "anolis/provider_sdk/result.hpp"
#include "config/provider_config.hpp"
#include "core/host_check.hpp"
#include "core/runtime_state.hpp"
#include "crumbs/session.hpp"
#include "devices/common/adapter_helpers.hpp"
#include "devices/common/device_type.hpp"
#include "protocol.pb.h"

extern "C" {
#include <bread/bread_watchdog.h>
#include <bread/dcmt_ops.h>
}

// BreadProviderRuntime tests — preserve the bread-specific coverage lost when
// core/handlers.cpp moved to the SDK: the inventory/read seam and the §8.3
// two-stage call() (build_frame validates args BEFORE the null-session guard).
// Driven in mock mode (mock:// bus_path → CrumbsTransport over a CrumbsCannedBus
// that answers real, CRC'd, padded CRUMBS wire bytes through the real decode).

namespace {

namespace adpp = anolis::deviceprovider::v1;

anolis_provider_bread::ProviderConfig make_mock_config() {
    anolis_provider_bread::ProviderConfig config;
    config.provider_name = "bread-unit-test";
    config.bus_path = "mock://unit-test";  // mock mode: no hardware
    config.discovery_mode = anolis_provider_bread::DiscoveryMode::Manual;
    config.manual_addresses = {0x08, 0x09};
    config.devices = {
        anolis_provider_bread::DeviceSpec{"rlht0", anolis_provider_bread::DeviceType::Rlht, "Left Heater", 0x08},
        anolis_provider_bread::DeviceSpec{"dcmt0", anolis_provider_bread::DeviceType::Dcmt, "Conveyor Drive", 0x09},
    };
    return config;
}

// Collects what the provider logs at info/warning level (std::clog).
class ClogCapture {
public:
    ClogCapture() : saved_(std::clog.rdbuf(captured_.rdbuf())) {}
    ~ClogCapture() { std::clog.rdbuf(saved_); }
    ClogCapture(const ClogCapture&) = delete;
    ClogCapture& operator=(const ClogCapture&) = delete;
    std::string text() const { return captured_.str(); }

private:
    std::ostringstream captured_;
    std::streambuf* saved_;
};

anolis_provider_bread::BreadProviderRuntime make_ready_runtime() {
    anolis_provider_bread::runtime::reset();
    anolis_provider_bread::runtime::initialize(make_mock_config());
    return {};
}

}  // namespace

TEST(BreadProviderRuntimeTest, InventoryAndMetadata) {
    const auto rt = make_ready_runtime();

    const auto meta = rt.metadata();
    EXPECT_EQ(meta.name, "anolis-provider-bread");
    EXPECT_EQ(meta.protocol_version, "v1");
    EXPECT_EQ(meta.hello_extra.at("supports_wait_ready"), "true");
    EXPECT_TRUE(meta.hello_extra.contains("inventory_mode"));

    EXPECT_TRUE(rt.has_device("rlht0"));
    EXPECT_FALSE(rt.has_device("nope"));
    EXPECT_EQ(rt.device_info("rlht0").device_id(), "rlht0");
    EXPECT_GT(rt.capabilities("rlht0").signals_size(), 0);

    // readiness must carry a real init_time_ms (a retired waiver depends on it).
    const auto r = rt.readiness();
    EXPECT_EQ(r.configured_device_count, 2);
    EXPECT_TRUE(r.extra_diagnostics.contains("init_time_ms"));
}

TEST(BreadProviderRuntimeTest, ReadDefaultSetThroughCannedBus) {
    auto rt = make_ready_runtime();

    // empty signal_ids -> the adapter's curated default set (§7.2); pass-through.
    const anolis::provider_sdk::AdapterReadResult result = rt.read("rlht0", {});
    ASSERT_TRUE(result.ok) << result.error_message;
    EXPECT_FALSE(result.values.empty());
    for (const auto& value : result.values) {
        EXPECT_FALSE(value.signal_id().empty());
        EXPECT_TRUE(value.has_timestamp());
    }

    const auto unknown = rt.read("ghost", {});
    EXPECT_FALSE(unknown.ok);
    EXPECT_EQ(unknown.error_code, adpp::Status::CODE_NOT_FOUND);
}

TEST(BreadProviderRuntimeTest, ReadDcmtStateThroughCannedBus) {
    auto rt = make_ready_runtime();

    // The canned DCMT state is the module major 2 layout, estop_count included.
    // Startup has stopped its motors (#195).
    const anolis::provider_sdk::AdapterReadResult result = rt.read("dcmt0", {"motor1_value", "estop_count"});
    ASSERT_TRUE(result.ok) << result.error_message;
    ASSERT_EQ(result.values.size(), 2u);
    EXPECT_EQ(result.values.at(0).value().int64_value(), 0);
    EXPECT_EQ(result.values.at(1).signal_id(), "estop_count");
    EXPECT_EQ(result.values.at(1).value().uint64_value(), 0u);
}

TEST(BreadProviderRuntimeTest, CallResolutionAndArgValidation) {
    auto rt = make_ready_runtime();

    // unknown function_id -> NOT_FOUND.
    EXPECT_EQ(rt.call("rlht0", 9999, {}).error_code, adpp::Status::CODE_NOT_FOUND);

    // §8.3: a valid function called with NO args -> build_frame rejects with an
    // arg error BEFORE any transmit (every RLHT function the seeded rlht0 offers
    // requires args). This exercises the validate-before-hardware contract
    // through the runtime.
    const auto caps = rt.capabilities("rlht0");  // own the CapabilitySet (no dangling ref into a temporary)
    ASSERT_GT(caps.functions_size(), 0);
    const auto& fn = caps.functions(0);
    const auto resolved = rt.resolve_function_id("rlht0", fn.name());
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(*resolved, fn.function_id());

    const auto bad_args = rt.call("rlht0", fn.function_id(), {});
    EXPECT_FALSE(bad_args.ok);
    EXPECT_TRUE(bad_args.error_code == adpp::Status::CODE_INVALID_ARGUMENT ||
                bad_args.error_code == adpp::Status::CODE_OUT_OF_RANGE)
        << "expected an arg error from build_frame, got " << bad_args.error_code << ": " << bad_args.error_message;

    EXPECT_FALSE(rt.resolve_function_id("rlht0", "no_such_fn").has_value());
}

TEST(BreadProviderRuntimeTest, ClearWatchdogTripCallableOnlyWhereAdvertised) {
    auto rt = make_ready_runtime();

    // The seeded DCMT models latching firmware (CMD_WATCHDOG + CLEAR_WATCHDOG_TRIP):
    // the zero-arg function resolves and a call goes through the canned bus.
    const auto dcmt_id = rt.resolve_function_id("dcmt0", "clear_watchdog_trip");
    ASSERT_TRUE(dcmt_id.has_value());
    const auto cleared = rt.call("dcmt0", *dcmt_id, {});
    EXPECT_TRUE(cleared.ok) << cleared.error_message;

    // The seeded RLHT is baseline (no watchdog caps): not offered, and its id is
    // rejected rather than sent.
    EXPECT_FALSE(rt.resolve_function_id("rlht0", "clear_watchdog_trip").has_value());
    EXPECT_EQ(rt.call("rlht0", 7, {}).error_code, adpp::Status::CODE_NOT_FOUND);
}

// #190: the boards are fed from arming until the runtime's first read or call
// of each device, which ends the feeding and logs how long it took.
TEST(BreadProviderRuntimeTest, FirstReadEndsTheWatchdogKeepalive) {
    auto config = make_mock_config();
    config.devices[1].command_watchdog_ms = 5000;  // dcmt0
    anolis_provider_bread::runtime::reset();
    anolis_provider_bread::runtime::initialize(config);
    anolis_provider_bread::BreadProviderRuntime rt;

    std::string log;
    {
        const ClogCapture capture;
        ASSERT_TRUE(rt.read("dcmt0", {}).ok);
        ASSERT_TRUE(rt.read("dcmt0", {}).ok);
        log = capture.text();
    }
    const std::string line = "watchdog: dcmt0 (0x09) first call ";
    ASSERT_NE(log.find(line), std::string::npos) << log;
    EXPECT_EQ(log.find(line, log.find(line) + 1), std::string::npos) << log;
    EXPECT_NE(log.find("fed every 1250 ms until then"), std::string::npos) << log;

    anolis_provider_bread::runtime::reset();
}

TEST(BreadProviderRuntimeTest, ArmedBoardsAreFedUntilTheFirstRead) {
    auto config = make_mock_config();
    config.devices[1].command_watchdog_ms = 40;  // dcmt0: fed every 10 ms
    anolis_provider_bread::runtime::reset();
    anolis_provider_bread::runtime::initialize(config);
    anolis_provider_bread::BreadProviderRuntime rt;
    anolis_provider_bread::crumbs::Session* session = anolis_provider_bread::runtime::session();
    ASSERT_NE(session, nullptr);

    // Nothing but the keepalive talks to the bus here.
    const uint64_t at_start = session->stats_for(0x09).ok;
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    EXPECT_GE(session->stats_for(0x09).ok, at_start + 2);

    ASSERT_TRUE(rt.read("dcmt0", {}).ok);
    const uint64_t after_read = session->stats_for(0x09).ok;
    std::this_thread::sleep_for(std::chrono::milliseconds{60});
    EXPECT_EQ(session->stats_for(0x09).ok, after_read) << "the first read ends the feeding";

    anolis_provider_bread::runtime::reset();
}

TEST(BreadProviderRuntimeTest, FirstCallEndsTheWatchdogKeepalive) {
    auto config = make_mock_config();
    config.devices[1].command_watchdog_ms = 5000;  // dcmt0
    anolis_provider_bread::runtime::reset();
    anolis_provider_bread::runtime::initialize(config);
    anolis_provider_bread::BreadProviderRuntime rt;
    const auto clear = rt.resolve_function_id("dcmt0", "clear_watchdog_trip");
    ASSERT_TRUE(clear.has_value());

    std::string log;
    {
        const ClogCapture capture;
        ASSERT_TRUE(rt.call("dcmt0", *clear, {}).ok);
        log = capture.text();
    }
    EXPECT_NE(log.find("watchdog: dcmt0 (0x09) first call "), std::string::npos) << log;

    anolis_provider_bread::runtime::reset();
}

TEST(BreadProviderRuntimeTest, SeededHealthOmitsModuleVersion) {
    // module_version reports what a device said at probe; mock inventory is
    // config-seeded, so no device said anything and the metric is absent even
    // though the descriptor still carries a module_version tag.
    const auto rt = make_ready_runtime();
    const auto health = rt.device_health("rlht0");
    EXPECT_EQ(health.metrics.count("module_version"), 0U);
    EXPECT_EQ(health.metrics.count("firmware_version"), 0U);
    EXPECT_EQ(health.metrics.at("type_id"), "bread.rlht");
}

TEST(BreadProviderRuntimeTest, HealthReadRearmsDeviceThatRebootedDisarmed) {
    // #148: a slice that reboots between polls comes back disarmed with no
    // failed operation, so the session's recovery signal never fires. The
    // health read sees armed=false and re-arms the device.
    auto config = make_mock_config();
    config.devices[1].command_watchdog_ms = 5000;  // dcmt0
    anolis_provider_bread::runtime::reset();
    anolis_provider_bread::runtime::initialize(config);
    const anolis_provider_bread::BreadProviderRuntime rt;

    EXPECT_EQ(rt.device_health("dcmt0").metrics.at("watchdog_armed"), "true");

    // Model the reboot: the device is disarmed and nothing failed on the bus.
    anolis_provider_bread::crumbs::Session* session = anolis_provider_bread::runtime::session();
    ASSERT_NE(session, nullptr);
    anolis_provider_bread::crumbs::RawFrame disarm;
    disarm.type_id = DCMT_TYPE_ID;
    disarm.opcode = BREAD_OP_SET_WATCHDOG;
    disarm.payload = {0, 0};
    ASSERT_TRUE(session->send(0x09, disarm));
    EXPECT_FALSE(session->take_recovery(0x09));

    // The read that saw the reboot reports what it read ...
    EXPECT_EQ(rt.device_health("dcmt0").metrics.at("watchdog_armed"), "false");
    // ... and the next one finds the device armed again.
    const auto after = rt.device_health("dcmt0");
    EXPECT_EQ(after.metrics.at("watchdog_armed"), "true");
    EXPECT_EQ(after.metrics.at("watchdog_timeout_ms"), "5000");

    anolis_provider_bread::runtime::reset();
}

TEST(BreadProviderRuntimeTest, HealthSkipsWatchdogQueryWhileDeviceIsFailing) {
    // #166: on a held power cut each watchdog query blocks about 1 s, so a
    // health request must not query a device whose last operation failed.
    auto config = make_mock_config();
    config.devices[1].command_watchdog_ms = 5000;  // dcmt0
    anolis_provider_bread::runtime::reset();
    anolis_provider_bread::runtime::initialize(config);
    auto rt = anolis_provider_bread::BreadProviderRuntime{};

    anolis_provider_bread::crumbs::Session* session = anolis_provider_bread::runtime::session();
    ASSERT_NE(session, nullptr);
    const auto io_count = [&]() {
        const auto stats = session->stats_for(0x09);
        return stats.ok + stats.failed;
    };

    // A query for a reply the canned device has none for fails, as a dead bus
    // would. (A bare read is no longer enough: the safe outputs' confirmation
    // leaves a query pending, #195.)
    anolis_provider_bread::crumbs::RawFrame frame;
    ASSERT_FALSE(session->query_read(0x09, 0x7E, frame));
    ASSERT_TRUE(session->last_op_failed(0x09));

    const auto before = io_count();
    const auto failing = rt.device_health("dcmt0");
    EXPECT_EQ(io_count(), before) << "health queried a device whose last operation failed";
    EXPECT_EQ(failing.metrics.count("watchdog_armed"), 0U);
    EXPECT_EQ(failing.metrics.count("watchdog_tripped"), 0U);
    EXPECT_EQ(failing.metrics.at("io_failed"), "1");

    // A successful read clears the failure, and health queries again.
    ASSERT_TRUE(rt.read("dcmt0", {}).ok);
    EXPECT_FALSE(session->last_op_failed(0x09));
    const auto recovered_before = io_count();
    const auto recovered = rt.device_health("dcmt0");
    EXPECT_GT(io_count(), recovered_before);
    EXPECT_EQ(recovered.metrics.at("watchdog_armed"), "true");

    anolis_provider_bread::runtime::reset();
}

TEST(BreadProviderRuntimeTest, HealthReportsHowSetsAreConfirmed) {
    // #134: a successful call carries no message to the runtime, so health is
    // where a device's confirmation mode shows. Seeded mock devices report the
    // provider's own CRUMBS version (GET_ACK capable) and baseline caps without
    // the REFUSES bit.
    const auto rt = make_ready_runtime();
    EXPECT_EQ(rt.device_health("rlht0").metrics.at("set_confirmation"), "delivery_only");
    EXPECT_EQ(rt.device_health("dcmt0").metrics.at("set_confirmation"), "delivery_only");
}

TEST(BreadProviderRuntimeTest, MockModeHasNoHostRequirements) {
    const auto rt = make_ready_runtime();
    EXPECT_TRUE(anolis_provider_bread::check_host(make_mock_config()).empty());

    const auto r = rt.readiness();
    EXPECT_TRUE(r.ready);
    EXPECT_EQ(r.extra_diagnostics.at("host_check"), "ok");
    EXPECT_FALSE(r.extra_diagnostics.contains("host_unmet"));
    EXPECT_FALSE(rt.provider_health().state.has_value());
}

#if defined(__linux__)
TEST(BreadProviderRuntimeTest, MissingBusStaysUpNotReady) {
    // A real (non-mock) bus path that does not exist: startup must not throw.
    // The provider stays up with no devices, every configured device reported
    // missing with the reason, and says why in readiness and provider health
    // (executable profile v1 §6) instead of exiting into a runtime crash loop.
    auto config = make_mock_config();
    config.bus_path = "/nonexistent-anolis-test/i2c-9";
    config.max_bus_hz = 50000;

    const auto reqs = anolis_provider_bread::check_host(config);
    ASSERT_EQ(reqs.size(), 3U);  // present, access, clock (a maximum is set)
    EXPECT_EQ(anolis::provider_sdk::host_check::exit_code(reqs), 1);

    anolis_provider_bread::runtime::reset();
    ASSERT_NO_THROW(anolis_provider_bread::runtime::initialize(config));
    const anolis_provider_bread::BreadProviderRuntime rt;

    EXPECT_TRUE(rt.list_device_ids().empty());
    EXPECT_EQ(anolis_provider_bread::runtime::session(), nullptr);

    const auto r = rt.readiness();
    EXPECT_FALSE(r.ready);
    EXPECT_EQ(r.extra_diagnostics.at("ready"), "false");
    EXPECT_EQ(r.extra_diagnostics.at("host_check"), "unmet");
    EXPECT_NE(r.extra_diagnostics.at("host_unmet").find("i2c.bus_present"), std::string::npos);
    ASSERT_EQ(r.failed_devices.size(), 2U);
    for (const auto& failed : r.failed_devices) {
        EXPECT_NE(failed.reason.find("host requirements unmet"), std::string::npos) << failed.reason;
    }

    const auto health = rt.provider_health();
    ASSERT_TRUE(health.state.has_value());
    EXPECT_EQ(*health.state, adpp::ProviderHealth::STATE_DEGRADED);
    ASSERT_TRUE(health.message.has_value());
    EXPECT_NE(health.message->find("i2c.bus_present"), std::string::npos) << *health.message;

    anolis_provider_bread::runtime::reset();
}
#endif

// A start-up that throws (here the mock bus refuses to open) leaves the
// provider up and not ready with the reason, as an unmet host requirement
// does, instead of escaping into the run-loop and exiting
// (anolishq/anolis-provider-bread#168).
TEST(BreadProviderRuntimeTest, AFailedStartUpLeavesTheProviderUpAndNotReady) {
    auto config = make_mock_config();
    config.retry_count = -1;  // fails the session's open-time validation

    anolis_provider_bread::runtime::reset();
    ASSERT_NO_THROW(anolis_provider_bread::runtime::initialize(config, std::chrono::milliseconds{10000}));
    const anolis_provider_bread::BreadProviderRuntime rt;

    EXPECT_TRUE(rt.list_device_ids().empty());
    EXPECT_EQ(anolis_provider_bread::runtime::session(), nullptr);
    const auto state = anolis_provider_bread::runtime::snapshot();
    EXPECT_FALSE(state.ready);
    EXPECT_EQ(state.startup_message.rfind("start-up failed: failed to open mock CRUMBS bus", 0), 0u)
        << state.startup_message;
    ASSERT_EQ(state.missing_expected_ids.size(), 2u);
}

// #195: startup drives each device's outputs to safe values, the sequence the
// bioreactor's safety.safe_state hooks use. The canned devices start with
// outputs on: a DCMT driving both motors, an RLHT with relay 1 energized.
namespace {

const anolis::deviceprovider::v1::Value* find_signal(const anolis::provider_sdk::AdapterReadResult& read,
                                                     const std::string& id) {
    for (const auto& value : read.values) {
        if (value.signal_id() == id) {
            return &value.value();
        }
    }
    return nullptr;
}

std::string metric(const anolis::provider_sdk::DeviceHealthExtra& health, const std::string& key) {
    const auto it = health.metrics.find(key);
    return it == health.metrics.end() ? "(absent)" : it->second;
}

}  // namespace

TEST(BreadProviderRuntimeTest, StartupLeavesTheMotorsStopped) {
    auto rt = make_ready_runtime();
    const auto read = rt.read("dcmt0", {"mode", "motor1_value", "motor2_value"});
    ASSERT_TRUE(read.ok) << read.error_message;
    ASSERT_NE(find_signal(read, "mode"), nullptr);
    EXPECT_EQ(find_signal(read, "mode")->string_value(), "open_loop");
    EXPECT_EQ(find_signal(read, "motor1_value")->int64_value(), 0);
    EXPECT_EQ(find_signal(read, "motor2_value")->int64_value(), 0);
}

TEST(BreadProviderRuntimeTest, StartupLeavesTheHeaterRelaysOff) {
    auto rt = make_ready_runtime();
    const auto read = rt.read("rlht0", {"mode", "relay1_on", "relay2_on", "setpoint1_c", "setpoint2_c"});
    ASSERT_TRUE(read.ok) << read.error_message;
    ASSERT_NE(find_signal(read, "mode"), nullptr);
    EXPECT_EQ(find_signal(read, "mode")->string_value(), "open_loop");
    EXPECT_FALSE(find_signal(read, "relay1_on")->bool_value());
    EXPECT_FALSE(find_signal(read, "relay2_on")->bool_value());
    EXPECT_DOUBLE_EQ(find_signal(read, "setpoint1_c")->double_value(), 0.0);
    EXPECT_DOUBLE_EQ(find_signal(read, "setpoint2_c")->double_value(), 0.0);
}

TEST(BreadProviderRuntimeTest, HealthSaysTheSafeOutputsWereSet) {
    const auto rt = make_ready_runtime();
    EXPECT_EQ(metric(rt.device_health("dcmt0"), "safe_outputs"), "set");
    EXPECT_EQ(metric(rt.device_health("rlht0"), "safe_outputs"), "set");
    EXPECT_FALSE(rt.device_health("dcmt0").state.has_value());
}

TEST(BreadProviderRuntimeTest, ASafeOutputThatCannotBeSetIsAFaultInHealth) {
    // Every bus operation NAKs, and in mock mode the safe writes are the first.
    anolis_provider_bread::runtime::reset();
    auto config = make_mock_config();
    config.bus_path = "mock://unit-test?nak_every=1";
    anolis_provider_bread::runtime::initialize(config);
    const anolis_provider_bread::BreadProviderRuntime rt;

    const auto health = rt.device_health("dcmt0");
    EXPECT_EQ(metric(health, "safe_outputs").rfind("not set: ", 0), 0u) << metric(health, "safe_outputs");
    ASSERT_TRUE(health.state.has_value());
    EXPECT_EQ(*health.state, adpp::DeviceHealth::STATE_FAULT);
    ASSERT_TRUE(health.message.has_value());
    EXPECT_NE(health.message->find("safe outputs not set"), std::string::npos) << *health.message;
    EXPECT_TRUE(anolis_provider_bread::runtime::snapshot().ready);  // still up, and says why
}

TEST(BreadProviderRuntimeTest, LeaveKeepsTheOutputsAsTheyWere) {
    anolis_provider_bread::runtime::reset();
    auto config = make_mock_config();
    config.outputs_on_startup = anolis_provider_bread::OutputsOnStartup::Leave;
    anolis_provider_bread::runtime::initialize(config);
    auto rt = anolis_provider_bread::BreadProviderRuntime{};

    const auto dcmt = rt.read("dcmt0", {"motor1_value"});
    ASSERT_TRUE(dcmt.ok) << dcmt.error_message;
    EXPECT_EQ(find_signal(dcmt, "motor1_value")->int64_value(), 100);
    const auto rlht = rt.read("rlht0", {"relay1_on"});
    ASSERT_TRUE(rlht.ok) << rlht.error_message;
    EXPECT_TRUE(find_signal(rlht, "relay1_on")->bool_value());
    EXPECT_EQ(metric(rt.device_health("dcmt0"), "safe_outputs"), "left");
    EXPECT_FALSE(rt.device_health("dcmt0").state.has_value());
}

namespace {

// Starts dcmt0's motors, then fails one operation on it, so the next
// successful one marks it recovered, as after a reboot or a power cut.
void run_the_motors_then_lose_contact(anolis_provider_bread::BreadProviderRuntime& rt) {
    anolis::provider_sdk::ValueMap args;
    args["motor1_pwm"] = anolis_provider_bread::make_int64_val(80);
    args["motor2_pwm"] = anolis_provider_bread::make_int64_val(80);
    const auto started = rt.call("dcmt0", 1u, args);  // set_open_loop
    ASSERT_TRUE(started.ok) << started.error_message;
    anolis_provider_bread::crumbs::RawFrame frame;
    ASSERT_FALSE(anolis_provider_bread::runtime::session()->query_read(0x09, 0x7E, frame));
}

}  // namespace

TEST(BreadProviderRuntimeTest, ADeviceThatRecoversIsDrivenSafeAgain) {
    auto rt = make_ready_runtime();
    run_the_motors_then_lose_contact(rt);

    const ClogCapture log;
    ASSERT_TRUE(rt.read("dcmt0", {}).ok);  // the recovery
    const auto after = rt.read("dcmt0", {"motor1_value", "motor2_value"});
    ASSERT_TRUE(after.ok) << after.error_message;
    EXPECT_EQ(find_signal(after, "motor1_value")->int64_value(), 0);
    EXPECT_EQ(find_signal(after, "motor2_value")->int64_value(), 0);
    EXPECT_NE(log.text().find("dcmt0: outputs set safe after recovery"), std::string::npos) << log.text();
    EXPECT_EQ(metric(rt.device_health("dcmt0"), "safe_outputs"), "set");
}

TEST(BreadProviderRuntimeTest, LeaveDoesNotTouchADeviceThatRecovers) {
    anolis_provider_bread::runtime::reset();
    auto config = make_mock_config();
    config.outputs_on_startup = anolis_provider_bread::OutputsOnStartup::Leave;
    anolis_provider_bread::runtime::initialize(config);
    auto rt = anolis_provider_bread::BreadProviderRuntime{};
    run_the_motors_then_lose_contact(rt);

    ASSERT_TRUE(rt.read("dcmt0", {}).ok);  // the recovery
    const auto after = rt.read("dcmt0", {"motor1_value"});
    ASSERT_TRUE(after.ok) << after.error_message;
    EXPECT_EQ(find_signal(after, "motor1_value")->int64_value(), 80);
}
