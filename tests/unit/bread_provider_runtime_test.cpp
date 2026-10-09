#include "core/bread_provider_runtime.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "anolis/provider_sdk/host_check.hpp"
#include "anolis/provider_sdk/result.hpp"
#include "config/provider_config.hpp"
#include "core/host_check.hpp"
#include "core/runtime_state.hpp"
#include "devices/common/device_type.hpp"
#include "protocol.pb.h"

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
