#include "devices/common/safe_outputs.hpp"

#include <string>

#include "devices/common/device_adapter.hpp"

namespace anolis_provider_bread::safe_outputs {

Result drive(crumbs::Session &session, const inventory::InventoryDevice &device) {
    const DeviceAdapter &adapter = adapter_for(device.type);
    Result result;
    for (const SafeOutputStep &step : adapter.safe_output_steps()) {
        const AdapterCallResult sent = call(adapter, &session, device, step.function_id, step.args);
        if (!sent.ok) {
            result.ok = false;
            if (!result.failures.empty()) {
                result.failures += "; ";
            }
            result.failures += step.name + ": " + sent.error_message;
        }
    }
    return result;
}

std::string to_metric(const Result &result) { return result.ok ? "set" : "not set: " + result.failures; }

}  // namespace anolis_provider_bread::safe_outputs
