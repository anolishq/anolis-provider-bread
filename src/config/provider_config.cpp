#include "config/provider_config.hpp"

/**
 * @file provider_config.cpp
 * @brief YAML parsing and semantic validation for anolis-provider-bread configuration.
 */

#include <filesystem>
#include <iomanip>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

#include <yaml-cpp/yaml.h>

namespace anolis_provider_bread {
namespace {

const std::regex kIdentifierPattern("^[A-Za-z0-9_.-]{1,64}$");

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

    const std::string value = node.Scalar();
    if(value.empty()) {
        throw std::runtime_error(field_name + " must not be empty");
    }
    return value;
}

void validate_identifier(const std::string &value, const std::string &field_name) {
    if(!std::regex_match(value, kIdentifierPattern)) {
        throw std::runtime_error(field_name + " must match ^[A-Za-z0-9_.-]{1,64}$");
    }
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

bool parse_bool_value(const YAML::Node &node, const std::string &field_name) {
    const std::string text = require_scalar(node, field_name);
    if(text == "true") {
        return true;
    }
    if(text == "false") {
        return false;
    }
    throw std::runtime_error(field_name + " must be true or false");
}

int parse_address_text(const std::string &text, const std::string &field_name) {
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

int parse_address_value(const YAML::Node &node, const std::string &field_name) {
    return parse_address_text(require_scalar(node, field_name), field_name);
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

DeviceType parse_device_type(const std::string &value) {
    if(value == "rlht") {
        return DeviceType::Rlht;
    }
    if(value == "dcmt") {
        return DeviceType::Dcmt;
    }

    throw std::runtime_error("Invalid devices[].type: '" + value + "'");
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

std::string to_string(DeviceType type) {
    switch(type) {
    case DeviceType::Rlht:
        return "rlht";
    case DeviceType::Dcmt:
        return "dcmt";
    }

    return "unknown";
}

std::string format_i2c_address(int address) {
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << address;
    return out.str();
}

ProviderConfig load_config(const std::string &path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch(const YAML::Exception &e) {
        throw std::runtime_error("Failed to parse config '" + path + "': " + e.what());
    }

    ensure_map(root, "root");
    // Keep the accepted schema narrow so config drift shows up immediately
    // instead of silently carrying unused keys into runtime behavior.
    reject_unknown_keys(root, "root", {"provider", "hardware", "discovery", "devices"});

    ProviderConfig config;
    config.config_file_path = std::filesystem::absolute(path).string();

    const YAML::Node provider_node = root["provider"];
    if(provider_node) {
        ensure_map(provider_node, "provider");
        reject_unknown_keys(provider_node, "provider", {"name"});
        if(provider_node["name"]) {
            config.provider_name = require_scalar(provider_node["name"], "provider.name");
            validate_identifier(config.provider_name, "provider.name");
        }
    }

    const YAML::Node hardware_node = root["hardware"];
    if(!hardware_node) {
        throw std::runtime_error("Missing required section: hardware");
    }
    ensure_map(hardware_node, "hardware");
    reject_unknown_keys(
        hardware_node,
        "hardware",
        {"bus_path", "require_live_session", "query_delay_us", "timeout_ms", "retry_count"});

    config.bus_path = require_scalar(hardware_node["bus_path"], "hardware.bus_path");
    if(hardware_node["require_live_session"]) {
        config.require_live_session =
            parse_bool_value(hardware_node["require_live_session"], "hardware.require_live_session");
    }
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
        // Manual discovery turns the address list into part of the contract:
        // duplicates and empty lists are rejected before any bus traffic occurs.
        if(!addresses_node || !addresses_node.IsSequence() || addresses_node.size() == 0) {
            throw std::runtime_error("discovery.addresses must be a non-empty sequence when discovery.mode=manual");
        }

        std::set<int> seen;
        for(std::size_t i = 0; i < addresses_node.size(); ++i) {
            const std::string field_name = "discovery.addresses[" + std::to_string(i) + "]";
            const int address = parse_address_value(addresses_node[i], field_name);
            if(!seen.insert(address).second) {
                throw std::runtime_error("Duplicate discovery address: " + format_i2c_address(address));
            }
            config.manual_addresses.push_back(address);
        }
    } else if(addresses_node && addresses_node.size() > 0) {
        throw std::runtime_error("discovery.addresses is only valid when discovery.mode=manual");
    }

    const YAML::Node devices_node = root["devices"];
    if(devices_node) {
        if(!devices_node.IsSequence()) {
            throw std::runtime_error("devices must be a sequence");
        }

        std::set<std::string> seen_ids;
        std::set<int> seen_addresses;
        for(std::size_t i = 0; i < devices_node.size(); ++i) {
            const YAML::Node device_node = devices_node[i];
            if(!device_node.IsMap()) {
                throw std::runtime_error("devices[" + std::to_string(i) + "] must be a map");
            }

            reject_unknown_keys(device_node, "devices[" + std::to_string(i) + "]", {"id", "type", "label", "address"});

            if(!device_node["id"]) {
                throw std::runtime_error("devices[" + std::to_string(i) + "].id is required");
            }
            if(!device_node["type"]) {
                throw std::runtime_error("devices[" + std::to_string(i) + "].type is required");
            }
            if(!device_node["address"]) {
                throw std::runtime_error("devices[" + std::to_string(i) + "].address is required");
            }

            DeviceSpec spec;
            spec.id = require_scalar(device_node["id"], "devices[" + std::to_string(i) + "].id");
            validate_identifier(spec.id, "devices[" + std::to_string(i) + "].id");
            spec.type = parse_device_type(require_scalar(device_node["type"], "devices[" + std::to_string(i) + "].type"));
            spec.label = device_node["label"]
                             ? require_scalar(device_node["label"], "devices[" + std::to_string(i) + "].label")
                             : spec.id;
            spec.address = parse_address_value(device_node["address"], "devices[" + std::to_string(i) + "].address");

            // IDs and addresses are unique within one provider config because
            // startup diagnostics and health output key off those identities.
            if(!seen_ids.insert(spec.id).second) {
                throw std::runtime_error("Duplicate devices[].id: '" + spec.id + "'");
            }
            if(!seen_addresses.insert(spec.address).second) {
                throw std::runtime_error("Duplicate devices[].address: '" + format_i2c_address(spec.address) + "'");
            }

            config.devices.push_back(spec);
        }
    }

    return config;
}

std::string summarize_config(const ProviderConfig &config) {
    std::ostringstream out;
    // The summary is intentionally compact and stable so startup logs can show
    // the effective config without dumping the full YAML file.
    out << "provider.name=" << config.provider_name
        << ", hardware.bus_path=" << config.bus_path
        << ", hardware.require_live_session=" << (config.require_live_session ? "true" : "false")
        << ", hardware.query_delay_us=" << config.query_delay_us
        << ", hardware.timeout_ms=" << config.timeout_ms
        << ", hardware.retry_count=" << config.retry_count
        << ", discovery.mode=" << to_string(config.discovery_mode)
        << ", devices=" << config.devices.size();

    if(config.discovery_mode == DiscoveryMode::Manual) {
        out << ", discovery.addresses=[";
        for(std::size_t i = 0; i < config.manual_addresses.size(); ++i) {
            if(i > 0) {
                out << ", ";
            }
            out << format_i2c_address(config.manual_addresses[i]);
        }
        out << "]";
    }

    return out.str();
}

} // namespace anolis_provider_bread
