#pragma once

#include <string>
#include <vector>

namespace anolis_provider_bread {

enum class DiscoveryMode {
    Scan,
    Manual,
};

enum class DeviceType {
    Rlht,
    Dcmt,
};

struct DeviceSpec {
    std::string id;
    DeviceType type = DeviceType::Rlht;
    std::string label;
    int address = 0;
};

struct ProviderConfig {
    std::string config_file_path;
    std::string provider_name = "anolis-provider-bread";
    std::string bus_path;
    bool require_live_session = false;
    int query_delay_us = 10000;
    int timeout_ms = 100;
    int retry_count = 2;
    DiscoveryMode discovery_mode = DiscoveryMode::Scan;
    std::vector<int> manual_addresses;
    std::vector<DeviceSpec> devices;
};

ProviderConfig load_config(const std::string &path);
DiscoveryMode parse_discovery_mode(const std::string &value);
DeviceType parse_device_type(const std::string &value);
std::string to_string(DiscoveryMode mode);
std::string to_string(DeviceType type);
std::string format_i2c_address(int address);
std::string summarize_config(const ProviderConfig &config);

} // namespace anolis_provider_bread
