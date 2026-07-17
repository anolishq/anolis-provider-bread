#include "core/bread_provider_runtime.hpp"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "anolis/provider_sdk/result.hpp"
#include "config/provider_config.hpp"
#include "core/runtime_state.hpp"
#include "crumbs/session.hpp"
#include "devices/common/device_adapter.hpp"
#include "devices/common/inventory.hpp"
#include "devices/common/watchdog.hpp"
#include "logging/logger.hpp"
#include "protocol.pb.h"

#ifndef ANOLIS_PROVIDER_BREAD_VERSION
#define ANOLIS_PROVIDER_BREAD_VERSION "0.0.0"
#endif

namespace anolis_provider_bread {
namespace {

namespace adpp = anolis::deviceprovider::v1;
namespace sdk = anolis::provider_sdk;

// After a successful operation, consume the session's address-recovery signal
// and re-apply per-device session state that a device reboot resets — today
// the firmware command watchdog (#112). Covers e-stop wirings that power-cycle
// the backplane: the board reboots disarmed and is re-armed on first contact.
void handle_recovery(crumbs::Session* session, const inventory::InventoryDevice& device) {
    if (session == nullptr || !session->take_recovery(static_cast<uint8_t>(device.address))) {
        return;
    }
    logging::info("device " + device.descriptor.device_id() + " (" + format_i2c_address(device.address) +
                  ") recovered after I/O failures");
    watchdog::arm_if_configured(*session, device, "recovery");
}

google::protobuf::Timestamp to_timestamp(const std::chrono::system_clock::time_point& time_point) {
    using namespace std::chrono;
    const auto seconds = time_point_cast<std::chrono::seconds>(time_point);
    const auto nanos = duration_cast<nanoseconds>(time_point - seconds);
    google::protobuf::Timestamp timestamp;
    timestamp.set_seconds(seconds.time_since_epoch().count());
    timestamp.set_nanos(static_cast<int>(nanos.count()));
    return timestamp;
}

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
    // When the address was probed and failed, the reason carries the probe
    // failure so operators see WHY without reading the provider log (#104).
    for (const auto& missing_id : state.missing_expected_ids) {
        const auto detail = state.missing_expected_details.find(missing_id);
        r.failed_devices.push_back({missing_id, "",
                                    detail == state.missing_expected_details.end()
                                        ? "expected device not found during startup"
                                        : detail->second});
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

sdk::DeviceHealthExtra BreadProviderRuntime::device_health(const std::string& device_id) const {
    // Per-device health (#87): last_seen is the wall-clock time of the last
    // successful CRUMBS operation against the device's address, and the io_*
    // metrics expose the session's cumulative counters — including retries
    // that eventually succeeded, so intermittent bus trouble is visible here
    // instead of hiding behind the retry policy.
    const runtime::RuntimeState state = runtime::snapshot();
    sdk::DeviceHealthExtra extra;
    if (const inventory::InventoryDevice* device = inventory::find_device(state.devices, device_id)) {
        extra.metrics["address"] = device->descriptor.address();
        extra.metrics["type_id"] = device->descriptor.type_id();
        extra.metrics["inventory"] = state.inventory_mode;
        if (crumbs::Session* session = runtime::session()) {
            const crumbs::AddressStats stats = session->stats_for(static_cast<uint8_t>(device->address));
            extra.metrics["io_ok"] = std::to_string(stats.ok);
            extra.metrics["io_failed"] = std::to_string(stats.failed);
            extra.metrics["io_retried_attempts"] = std::to_string(stats.retried_attempts);
            if (stats.has_success) {
                extra.last_seen = to_timestamp(stats.last_success);
            }
            // Watchdog status is a live bus query (there is no passive source
            // for trip state); only devices configured to arm pay for it, and
            // a failed query just omits the metrics rather than failing health.
            if (device->command_watchdog_ms > 0 && watchdog::capability_supported(*device)) {
                watchdog::WatchdogStatus wd;
                if (watchdog::query_status(*session, *device, wd)) {
                    extra.metrics["watchdog_armed"] = wd.armed ? "true" : "false";
                    extra.metrics["watchdog_timeout_ms"] = std::to_string(wd.timeout_ms);
                    extra.metrics["watchdog_tripped"] = wd.tripped ? "true" : "false";
                    extra.metrics["watchdog_trip_count"] = std::to_string(wd.trip_count);
                }
            }
        }
        // No successful contact yet -> last_seen stays unset (never fabricated).
        return extra;
    }
    // Missing-expected devices surface on get_health (readiness() maps them to
    // failed_devices); they are NOT in the live inventory, so branch on the
    // missing-expected set rather than a find_device lookup (which returns null).
    // There has been no contact with a missing device, so last_seen stays unset.
    for (const auto& missing_id : state.missing_expected_ids) {
        if (missing_id == device_id) {
            extra.metrics["inventory"] = state.inventory_mode;
            extra.metrics["missing"] = "true";
            const auto detail = state.missing_expected_details.find(device_id);
            if (detail != state.missing_expected_details.end()) {
                extra.metrics["missing_detail"] = detail->second;
            }
            break;
        }
    }
    return extra;
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
    if (br.ok) {
        handle_recovery(session, *device);
    }
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
    if (br.ok) {
        handle_recovery(runtime::session(), *device);
    }
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
