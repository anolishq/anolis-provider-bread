#pragma once

/**
 * @file bread_provider_runtime.hpp
 * @brief bread's implementation of the shared SDK `ProviderRuntime` seam.
 *
 * Thin, stateless adapter over the `runtime::` process singleton + the existing
 * device descriptor. `read()`/`call()` delegate to `adapter_for(type)` and the
 * §8.3 two-stage `call()` composer (build_frame validates BEFORE transmit's
 * null-session guard); the `crumbs::Session` handle is fetched fresh per request
 * via `runtime::session()` and never leaves the runtime — the SDK never sees
 * CRUMBS. bread keeps its own `DeviceAdapter`/`adapter_for` descriptor (over
 * `crumbs::Session&`); it does not instantiate the SDK's `DeviceAdapter<HandleT>`.
 * Mock vs hardware is a transport choice at `runtime::initialize()`, invisible here.
 */

#include "anolis/provider_sdk/runtime.hpp"

namespace anolis_provider_bread {

class BreadProviderRuntime : public anolis::provider_sdk::ProviderRuntime {
public:
    anolis::provider_sdk::ProviderMetadata metadata() const override;
    anolis::provider_sdk::ReadinessReport readiness() const override;
    std::vector<std::string> list_device_ids() const override;
    bool has_device(const std::string& device_id) const override;
    anolis::deviceprovider::v1::Device device_info(const std::string& device_id) const override;
    anolis::deviceprovider::v1::CapabilitySet capabilities(const std::string& device_id) const override;
    anolis::provider_sdk::AdapterReadResult read(const std::string& device_id,
                                                 const std::vector<std::string>& signal_ids) override;
    anolis::provider_sdk::AdapterCallResult call(const std::string& device_id, uint32_t function_id,
                                                 const anolis::provider_sdk::ValueMap& args) override;
    std::optional<uint32_t> resolve_function_id(const std::string& device_id,
                                                const std::string& function_name) const override;
};

}  // namespace anolis_provider_bread
