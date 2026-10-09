#include "core/host_check.hpp"

#include "anolis/provider_sdk/i2c/host_checks.hpp"

namespace anolis_provider_bread {

namespace hc = anolis::provider_sdk::host_check;

std::vector<hc::Requirement> check_host(const ProviderConfig &config) {
    if (config.bus_path.rfind("mock://", 0) == 0) {
        return {};
    }
    anolis::provider_sdk::i2c::HostCheckOptions options;
    options.max_bus_hz = config.max_bus_hz;
    return anolis::provider_sdk::i2c::check_host(config.bus_path, options);
}

std::string summarize_unmet(const std::vector<hc::Requirement> &requirements) {
    const auto diagnostics = hc::readiness_diagnostics(requirements);
    const auto it = diagnostics.find("host_unmet");
    return it == diagnostics.end() ? std::string() : it->second;
}

}  // namespace anolis_provider_bread
