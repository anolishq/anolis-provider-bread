#include "config/provider_config.hpp"

#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

#include <yaml-cpp/yaml.h>

namespace anolis_provider_bread {
namespace {

void ensure_map(const YAML::Node &node, const std::string &field_name) {
    if(!node || !node.IsMap()) {
        throw std::runtime_error(field_name + " must be a map");
    }
}

void reject_unknown_keys(const YAML::Node &node,
                         const std::string &field_name,
                         const std::set<std::string> &allowed_keys) {
    for(const auto &entry : node) {
        const std::string key = entry.first.as<std::string>();
        if(allowed_keys.find(key) == allowed_keys.end()) {
            throw std::runtime_error("Unknown " + field_name + " key: '" + key + "'");
        }
    }
}

std::string require_scalar(const YAML::Node &node, const std::string &field_name) {
    if(!node || !node.IsScalar()) {
        throw std::runtime_error(field_name + " must be a scalar");
    }

    const std::string value = node.as<std::string>();
    if(value.empty()) {
        throw std::runtime_error(field_name + " must not be empty");
    }
    return value;
}

int parse_int_value(const YAML::Node &node,
                    const std::string &field_name,
                    bool allow_zero) {
    const std::string text = require_scalar(node, field_name);

    try {
        const int value = std::stoi(text, nullptr, 10);
        if(value < 0 || (!allow_zero && value == 0)) {
            throw std::runtime_error(field_name + " must be " +
                                     std::string(allow_zero ? "non-negative" : "positive"));
        }
        return value;
    } catch(const std::invalid_argument &) {
        throw std::runtime_error(field_name + " must be an integer");
    } catch(const std::out_of_range &) {
        throw std::runtime_error(field_name + " is out of range");
    }
}

int parse_address_value(const YAML::Node &node, const std::string &field_name) {
    const std::string text = require_scalar(node, field_name);
    const int base = (text.size() > 2 && text[0] == '0' &&
                      (text[1] == 'x' || text[1] == 'X'))
                         ? 16
                         : 10;

    try {
        const int value = std::stoi(text, nullptr, base);
        if(value < 0x08 || value > 0x77) {
            throw std::runtime_error(field_name + " must be in the 0x08-0x77 I2C address range");
        }
        return value;
    } catch(const std::invalid_argument &) {
        throw std::runtime_error(field_name + " must be an integer or hex literal");
    } catch(const std::out_of_range &) {
        throw std::runtime_error(field_name + " is out of range");
    }
}

} // namespace

DiscoveryMode parse_discovery_mode(const std::string &value) {
    if(value == "scan") {
        return DiscoveryMode::Scan;
    }
    if(value == "manual") {
        return DiscoveryMode::Manual;
    }

    throw std::runtime_error("Invalid discovery.mode: '" + value + "'");
}

std::string to_string(DiscoveryMode mode) {
    switch(mode) {
    case DiscoveryMode::Scan:
        return "scan";
    case DiscoveryMode::Manual:
        return "manual";
    }

    return "unknown";
}

ProviderConfig load_config(const std::string &path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch(const YAML::Exception &e) {
        throw std::runtime_error("Failed to parse config '" + path + "': " + e.what());
    }

    ensure_map(root, "root");
    reject_unknown_keys(root, "root", {"provider", "hardware", "discovery"});

    ProviderConfig config;
    config.config_file_path = path;

    const YAML::Node provider_node = root["provider"];
    if(provider_node) {
        ensure_map(provider_node, "provider");
        reject_unknown_keys(provider_node, "provider", {"name"});
        if(provider_node["name"]) {
            config.provider_name = require_scalar(provider_node["name"], "provider.name");
        }
    }

    const YAML::Node hardware_node = root["hardware"];
    if(!hardware_node) {
        throw std::runtime_error("Missing required section: hardware");
    }
    ensure_map(hardware_node, "hardware");
    reject_unknown_keys(hardware_node, "hardware", {"bus_path", "query_delay_us", "timeout_ms", "retry_count"});

    config.bus_path = require_scalar(hardware_node["bus_path"], "hardware.bus_path");
    if(hardware_node["query_delay_us"]) {
        config.query_delay_us = parse_int_value(hardware_node["query_delay_us"], "hardware.query_delay_us", false);
    }
    if(hardware_node["timeout_ms"]) {
        config.timeout_ms = parse_int_value(hardware_node["timeout_ms"], "hardware.timeout_ms", false);
    }
    if(hardware_node["retry_count"]) {
        config.retry_count = parse_int_value(hardware_node["retry_count"], "hardware.retry_count", true);
    }

    const YAML::Node discovery_node = root["discovery"];
    if(!discovery_node) {
        throw std::runtime_error("Missing required section: discovery");
    }
    ensure_map(discovery_node, "discovery");
    reject_unknown_keys(discovery_node, "discovery", {"mode", "addresses"});

    config.discovery_mode = parse_discovery_mode(require_scalar(discovery_node["mode"], "discovery.mode"));

    const YAML::Node addresses_node = discovery_node["addresses"];
    if(config.discovery_mode == DiscoveryMode::Manual) {
        if(!addresses_node || !addresses_node.IsSequence() || addresses_node.size() == 0) {
            throw std::runtime_error("discovery.addresses must be a non-empty sequence when discovery.mode=manual");
        }

        std::set<int> seen;
        for(std::size_t i = 0; i < addresses_node.size(); ++i) {
            const std::string field_name = "discovery.addresses[" + std::to_string(i) + "]";
            const int address = parse_address_value(addresses_node[i], field_name);
            if(!seen.insert(address).second) {
                throw std::runtime_error("Duplicate discovery address: " + std::to_string(address));
            }
            config.manual_addresses.push_back(address);
        }
    } else if(addresses_node && addresses_node.size() > 0) {
        throw std::runtime_error("discovery.addresses is only valid when discovery.mode=manual");
    }

    return config;
}

std::string summarize_config(const ProviderConfig &config) {
    std::ostringstream out;
    out << "provider.name=" << config.provider_name
        << ", hardware.bus_path=" << config.bus_path
        << ", hardware.query_delay_us=" << config.query_delay_us
        << ", hardware.timeout_ms=" << config.timeout_ms
        << ", hardware.retry_count=" << config.retry_count
        << ", discovery.mode=" << to_string(config.discovery_mode);

    if(config.discovery_mode == DiscoveryMode::Manual) {
        out << ", discovery.addresses=[";
        for(std::size_t i = 0; i < config.manual_addresses.size(); ++i) {
            if(i > 0) {
                out << ", ";
            }
            out << config.manual_addresses[i];
        }
        out << "]";
    }

    return out.str();
}

} // namespace anolis_provider_bread
