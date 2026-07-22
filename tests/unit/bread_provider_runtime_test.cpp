#include "core/bread_provider_runtime.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "anolis/provider_sdk/result.hpp"
#include "config/provider_config.hpp"
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
    // arg error BEFORE any transmit (every bread function requires args; there is
    // no zero-arg function — that's the §8.1 conformance skip). This exercises the
    // validate-before-hardware contract through the runtime.
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
