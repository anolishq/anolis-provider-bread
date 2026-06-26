#include "core/bread_provider_runtime.hpp"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "anolis/provider_sdk/result.hpp"
#include "config/provider_config.hpp"
#include "core/runtime_state.hpp"
#include "devices/common/device_adapter.hpp"
#include "devices/common/inventory.hpp"
#include "protocol.pb.h"

#ifndef ANOLIS_PROVIDER_BREAD_VERSION
#define ANOLIS_PROVIDER_BREAD_VERSION "0.0.0"
#endif

namespace anolis_provider_bread {
namespace {

namespace adpp = anolis::deviceprovider::v1;
namespace sdk = anolis::provider_sdk;

}  // namespace

sdk::ProviderMetadata BreadProviderRuntime::metadata() const {
    sdk::ProviderMetadata meta;
    meta.name = "anolis-provider-bread";
    meta.version = ANOLIS_PROVIDER_BREAD_VERSION;
    meta.protocol_version = "v1";
    // The SDK's handle_hello injects transport + max_frame_bytes; supply only
    // bread's extra advertisements (preserves the pre-migration Hello metadata).
    meta.hello_extra["supports_wait_ready"] = "true";
    meta.hello_extra["inventory_mode"] = runtime::snapshot().inventory_mode;
    return meta;
}

sdk::ReadinessReport BreadProviderRuntime::readiness() const {
    const runtime::RuntimeState state = runtime::snapshot();
    sdk::ReadinessReport r;
    r.ready = state.ready;
    r.configured_device_count = static_cast<int>(state.config.devices.size());
    for (const auto& device : state.devices) {
        r.successful_device_ids.push_back(device.descriptor.device_id());
    }
    // Expected-but-missing devices remain visible as STATE_UNREACHABLE health.
    for (const auto& missing_id : state.missing_expected_ids) {
        r.failed_devices.push_back({missing_id, "", "expected device not found during startup"});
    }
    r.startup_policy = "degraded";  // bread serves discovered devices, flags missing-expected
    r.provider_impl = "bread";

    // Preserve bread's WaitReady diagnostics surface (the SDK merges extra_diagnostics).
    // init_time_ms MUST be the real elapsed value (a retired waiver depends on it).
    const auto ready_point = state.ready ? state.ready_at : std::chrono::system_clock::now();
    const auto init_ms = std::chrono::duration_cast<std::chrono::milliseconds>(ready_point - state.started_at).count();
    auto& diag = r.extra_diagnostics;
    diag["init_time_ms"] = std::to_string(init_ms < 0 ? 0 : init_ms);
    diag["ready"] = state.ready ? "true" : "false";
    // "degraded" is the pre-migration semantic: ready but serving a reduced roster.
    // Not-yet-ready is "starting", not "degraded". (The SDK's separate
    // "startup_degraded" key carries the broader missing-expected signal.)
    diag["degraded"] = (state.ready && !state.missing_expected_ids.empty()) ? "true" : "false";
    diag["inventory_mode"] = state.inventory_mode;
    diag["discovery_mode"] = to_string(state.config.discovery_mode);
    diag["bus_path"] = state.config.bus_path;
    diag["query_delay_us"] = std::to_string(state.config.query_delay_us);
    diag["unsupported_probe_count"] = std::to_string(state.unsupported_probe_count);
    diag["missing_expected_count"] = std::to_string(state.missing_expected_ids.size());
    diag["startup_message"] = state.startup_message;
    return r;
}

std::vector<std::string> BreadProviderRuntime::list_device_ids() const {
    const runtime::RuntimeState state = runtime::snapshot();
    std::vector<std::string> ids;
    ids.reserve(state.devices.size());
    for (const auto& device : state.devices) {
        ids.push_back(device.descriptor.device_id());
    }
    return ids;
}

bool BreadProviderRuntime::has_device(const std::string& device_id) const {
    const runtime::RuntimeState state = runtime::snapshot();
    return inventory::find_device(state.devices, device_id) != nullptr;
}

adpp::Device BreadProviderRuntime::device_info(const std::string& device_id) const {
    const runtime::RuntimeState state = runtime::snapshot();
    const inventory::InventoryDevice* device = inventory::find_device(state.devices, device_id);
    return device == nullptr ? adpp::Device{} : device->descriptor;
}

adpp::CapabilitySet BreadProviderRuntime::capabilities(const std::string& device_id) const {
    const runtime::RuntimeState state = runtime::snapshot();
    const inventory::InventoryDevice* device = inventory::find_device(state.devices, device_id);
    return device == nullptr ? adpp::CapabilitySet{} : device->capabilities;
}

sdk::AdapterReadResult BreadProviderRuntime::read(const std::string& device_id,
                                                  const std::vector<std::string>& signal_ids) {
    const runtime::RuntimeState state = runtime::snapshot();
    const inventory::InventoryDevice* device = inventory::find_device(state.devices, device_id);
    if (device == nullptr) {
        return {false, adpp::Status::CODE_NOT_FOUND, "unknown device_id", {}};
    }
    crumbs::Session* session = runtime::session();
    if (session == nullptr) {
        return {false,
                adpp::Status::CODE_UNAVAILABLE,
                "no hardware session (provider not built with hardware support)",
                {}};
    }

    // Pass signal_ids THROUGH — the §7.2 default-set expansion lives inside the
    // adapter (should_include / is_default_signal), not here.
    AdapterReadResult br = adapter_for(device->type).read_signals(*session, *device, signal_ids);
    sdk::AdapterReadResult out;
    out.ok = br.ok;
    out.error_code = br.error_code;
    out.error_message = std::move(br.error_message);
    out.values = std::move(br.values);
    return out;
}

sdk::AdapterCallResult BreadProviderRuntime::call(const std::string& device_id, uint32_t function_id,
                                                  const sdk::ValueMap& args) {
    const runtime::RuntimeState state = runtime::snapshot();
    const inventory::InventoryDevice* device = inventory::find_device(state.devices, device_id);
    if (device == nullptr) {
        return sdk::call_not_found("unknown device_id");
    }
    // The SDK already resolved §6.2 (function_id is final); verify it exists.
    if (!inventory::function_exists(*device, function_id, "")) {
        return sdk::call_not_found("unknown function_id");
    }

    // §8.3: delegate to bread's existing two-stage composer (qualified to avoid
    // colliding with this method). build_frame validates args BEFORE the null-
    // session guard, so a null session yields INVALID_ARGUMENT/OUT_OF_RANGE on
    // bad args and CODE_UNAVAILABLE only on valid-but-no-hardware.
    AdapterCallResult br =
        anolis_provider_bread::call(adapter_for(device->type), runtime::session(), *device, function_id, args);
    return {br.ok, br.error_code, std::move(br.error_message)};
}

std::optional<uint32_t> BreadProviderRuntime::resolve_function_id(const std::string& device_id,
                                                                  const std::string& function_name) const {
    const runtime::RuntimeState state = runtime::snapshot();
    const inventory::InventoryDevice* device = inventory::find_device(state.devices, device_id);
    if (device == nullptr) {
        return std::nullopt;
    }
    for (const auto& function : device->capabilities.functions()) {
        if (function.name() == function_name) {
            return function.function_id();
        }
    }
    return std::nullopt;
}

}  // namespace anolis_provider_bread
