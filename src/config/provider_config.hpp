#pragma once

#include <string>
#include <vector>

namespace anolis_provider_bread {

enum class DiscoveryMode {
    Scan,
    Manual,
};

struct ProviderConfig {
    std::string config_file_path;
    std::string provider_name = "anolis-provider-bread";
    std::string bus_path;
    int query_delay_us = 10000;
    int timeout_ms = 100;
    int retry_count = 2;
    DiscoveryMode discovery_mode = DiscoveryMode::Scan;
    std::vector<int> manual_addresses;
};

ProviderConfig load_config(const std::string &path);
DiscoveryMode parse_discovery_mode(const std::string &value);
std::string to_string(DiscoveryMode mode);
std::string summarize_config(const ProviderConfig &config);

} // namespace anolis_provider_bread
