#include "core/health.hpp"

/**
 * @file health.cpp
 * @brief Health helpers that project BREAD startup and inventory state into ADPP responses.
 */

#include <chrono>
#include <string>

#include <google/protobuf/timestamp.pb.h>

namespace anolis_provider_bread::health {
namespace {

google::protobuf::Timestamp to_timestamp(const std::chrono::system_clock::time_point &time_point) {
    using namespace std::chrono;
    const auto seconds = time_point_cast<std::chrono::seconds>(time_point);
    const auto nanos = duration_cast<nanoseconds>(time_point - seconds);

    google::protobuf::Timestamp timestamp;
    timestamp.set_seconds(seconds.time_since_epoch().count());
    timestamp.set_nanos(static_cast<int>(nanos.count()));
    return timestamp;
}

} // namespace

ProviderHealth make_provider_health(const runtime::RuntimeState &state) {
    const bool degraded = state.ready && !state.missing_expected_ids.empty();
    ProviderHealth provider;
    // The provider is considered degraded when startup succeeded but the
    // configured expected roster could not be fully satisfied.
    provider.set_state(!state.ready || degraded
                           ? ProviderHealth::STATE_DEGRADED
                           : ProviderHealth::STATE_OK);
    if (degraded) {
        provider.set_message("provider ready but " +
                             std::to_string(state.missing_expected_ids.size()) +
                             " expected device(s) not found");
    } else {
        provider.set_message(state.startup_message);
    }
    provider.mutable_metrics()->insert({"device_count", std::to_string(state.devices.size())});
    provider.mutable_metrics()->insert({"ready", state.ready ? "true" : "false"});
    provider.mutable_metrics()->insert({"degraded", degraded ? "true" : "false"});
    provider.mutable_metrics()->insert({"discovery_mode", to_string(state.config.discovery_mode)});
    provider.mutable_metrics()->insert({"inventory_mode", state.inventory_mode});
    provider.mutable_metrics()->insert({"bus_path", state.config.bus_path});
    provider.mutable_metrics()->insert({"unsupported_probe_count",
                                        std::to_string(state.unsupported_probe_count)});
    provider.mutable_metrics()->insert({"missing_expected_count",
                                        std::to_string(state.missing_expected_ids.size())});
    return provider;
}

std::vector<DeviceHealth> make_device_health(const runtime::RuntimeState &state) {
    std::vector<DeviceHealth> devices;
    devices.reserve(state.devices.size() + state.missing_expected_ids.size());

    for(const auto &device : state.devices) {
        DeviceHealth health;
        health.set_device_id(device.descriptor.device_id());
        // Published inventory devices are treated as reachable only when the
        // provider completed startup with a live or seeded runtime state.
        health.set_state(state.ready ? DeviceHealth::STATE_OK : DeviceHealth::STATE_UNREACHABLE);
        health.set_message(state.ready ? "ok" : "Provider not ready");
        *health.mutable_last_seen() = to_timestamp(state.started_at);
        health.mutable_metrics()->insert({"address", device.descriptor.address()});
        health.mutable_metrics()->insert({"type_id", device.descriptor.type_id()});
        health.mutable_metrics()->insert({"inventory", state.inventory_mode});
        devices.push_back(health);
    }

    for (const auto &id : state.missing_expected_ids) {
        DeviceHealth health;
        health.set_device_id(id);
        // Missing expected devices remain visible in health output so operators
        // can distinguish a degraded startup from a smaller intentional roster.
        health.set_state(DeviceHealth::STATE_UNREACHABLE);
        health.set_message("expected device not found during startup");
        *health.mutable_last_seen() = to_timestamp(state.started_at);
        health.mutable_metrics()->insert({"inventory", state.inventory_mode});
        health.mutable_metrics()->insert({"missing", "true"});
        devices.push_back(health);
    }

    return devices;
}

void populate_wait_ready(const runtime::RuntimeState &state, WaitReadyResponse &response) {
    const bool degraded = state.ready && !state.missing_expected_ids.empty();
    // WaitReady reports the same startup-era diagnostics that drive provider
    // health so callers can make readiness decisions without a second API.
    response.mutable_diagnostics()->insert({"provider_version", ANOLIS_PROVIDER_BREAD_VERSION});
    response.mutable_diagnostics()->insert({"device_count", std::to_string(state.devices.size())});
    response.mutable_diagnostics()->insert({"ready", state.ready ? "true" : "false"});
    response.mutable_diagnostics()->insert({"degraded", degraded ? "true" : "false"});
    response.mutable_diagnostics()->insert({"inventory_mode", state.inventory_mode});
    response.mutable_diagnostics()->insert({"discovery_mode", to_string(state.config.discovery_mode)});
    response.mutable_diagnostics()->insert({"bus_path", state.config.bus_path});
    response.mutable_diagnostics()->insert({"query_delay_us", std::to_string(state.config.query_delay_us)});
    response.mutable_diagnostics()->insert({"unsupported_probe_count",
                                            std::to_string(state.unsupported_probe_count)});
    response.mutable_diagnostics()->insert({"missing_expected_count",
                                            std::to_string(state.missing_expected_ids.size())});
    response.mutable_diagnostics()->insert({"startup_message", state.startup_message});
}

} // namespace anolis_provider_bread::health
