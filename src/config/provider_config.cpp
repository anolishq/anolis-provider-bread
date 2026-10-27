#include "config/provider_config.hpp"

/**
 * @file provider_config.cpp
 * @brief Config loading for anolis-provider-bread, driven by the declare-once
 * schema (config_schema.cpp).
 *
 * Validation runs the SDK validator against the SAME schema `--config-schema`
 * advertises, and value extraction uses the SDK's typed helpers (the same
 * scalar resolver as the validator) — so the advertised contract, the enforced
 * validation, and the parsed values cannot drift apart.
 */

#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <format>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

#include "anolis/provider_sdk/config_validate.hpp"
#include "config/config_schema.hpp"
#include "logging/logger.hpp"

namespace anolis_provider_bread {

namespace sdkcfg = anolis::provider_sdk::config;

DiscoveryMode parse_discovery_mode(const std::string &value) {
    if (value == "scan") {
        return DiscoveryMode::Scan;
    }
    if (value == "manual") {
        return DiscoveryMode::Manual;
    }

    throw std::runtime_error("Invalid discovery.mode: '" + value + "'");
}

OutputsOnStartup parse_outputs_on_startup(const std::string &value) {
    if (value == "safe") {
        return OutputsOnStartup::Safe;
    }
    if (value == "leave") {
        return OutputsOnStartup::Leave;
    }

    throw std::runtime_error("Invalid provider.outputs_on_startup: '" + value + "'");
}

DeviceType parse_device_type(const std::string &value) {
    if (value == "rlht") {
        return DeviceType::Rlht;
    }
    if (value == "dcmt") {
        return DeviceType::Dcmt;
    }

    throw std::runtime_error("Invalid devices[].type: '" + value + "'");
}

std::string to_string(DiscoveryMode mode) {
    switch (mode) {
        case DiscoveryMode::Scan:
            return "scan";
        case DiscoveryMode::Manual:
            return "manual";
    }

    return "unknown";
}

std::string to_string(OutputsOnStartup policy) {
    switch (policy) {
        case OutputsOnStartup::Safe:
            return "safe";
        case OutputsOnStartup::Leave:
            return "leave";
    }

    return "unknown";
}

std::string to_string(DeviceType type) {
    switch (type) {
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

namespace {

// The one exception to validating the config as written. Workbench 0.20.0 and
// later seeded `encoders: true` on every new device row, RLHT included, before
// the schema forbade it there (#184). It never had an effect on an RLHT, so it
// is dropped with a warning and those configs still load.
void drop_rlht_encoders(YAML::Node &root) {
    // A missing key reads as an invalid node, whose IsSequence() throws: test
    // that it is defined first. Reads go through const nodes, which add nothing.
    const YAML::Node &read = root;
    if (!read.IsMap() || !read["devices"] || !read["devices"].IsSequence()) {
        return;
    }
    for (YAML::Node device : root["devices"]) {
        const YAML::Node &fields = device;
        if (!fields.IsMap() || !fields["encoders"] || !fields["type"] || !fields["type"].IsScalar() ||
            fields["type"].Scalar() != "rlht") {
            continue;
        }
        const std::string id = fields["id"] && fields["id"].IsScalar() ? fields["id"].Scalar() : "(no id)";
        logging::warning(
            std::format("device '{}': encoders applies to DCMT only and is ignored on an RLHT; "
                        "remove it from the config",
                        id));
        device.remove("encoders");
    }
}

}  // namespace

ProviderConfig load_config(const std::string &path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::Exception &e) {
        throw std::runtime_error("Failed to parse config '" + path + "': " + e.what());
    }

    drop_rlht_encoders(root);

    // Declare-once validation: every structural/semantic error, collected at
    // once, against the schema `--config-schema` advertises.
    const auto errors = sdkcfg::validate(provider_schema(), root);
    if (!errors.empty()) {
        throw std::runtime_error(std::format("Invalid config '{}':\n{}", path, sdkcfg::format_errors(errors)));
    }

    ProviderConfig config;
    config.config_file_path = std::filesystem::absolute(path).string();

    // Post-validation extraction with the SDK's typed helpers — validation
    // guarantees presence/type for required fields, so absent optionals are
    // the only nullopt cases here.
    if (const auto name = sdkcfg::as_string(root["provider"]["name"])) {
        config.provider_name = *name;
    }
    if (const auto policy = sdkcfg::as_string(root["provider"]["outputs_on_startup"])) {
        config.outputs_on_startup = parse_outputs_on_startup(*policy);
    }

    const YAML::Node hardware = root["hardware"];
    if (const auto bus_path = sdkcfg::as_string(hardware["bus_path"])) {
        config.bus_path = *bus_path;
    }
    if (const auto value = sdkcfg::as_int64(hardware["query_delay_us"])) {
        config.query_delay_us = static_cast<int>(*value);
    }
    if (const auto value = sdkcfg::as_int64(hardware["timeout_ms"])) {
        config.timeout_ms = static_cast<int>(*value);
    }
    if (const auto value = sdkcfg::as_int64(hardware["retry_count"])) {
        config.retry_count = static_cast<int>(*value);
    }
    if (const auto value = sdkcfg::as_int64(hardware["max_bus_hz"])) {
        config.max_bus_hz = static_cast<uint32_t>(*value);
    }

    const YAML::Node discovery = root["discovery"];
    if (const auto mode = sdkcfg::as_string(discovery["mode"])) {
        config.discovery_mode = parse_discovery_mode(*mode);
    }
    const YAML::Node addresses = discovery["addresses"];
    if (addresses.IsDefined() && addresses.IsSequence()) {
        for (const auto &address_node : addresses) {
            if (const auto address = sdkcfg::parse_i2c_address(address_node)) {
                config.manual_addresses.push_back(*address);
            }
        }
    }

    const YAML::Node devices = root["devices"];
    if (devices.IsDefined() && devices.IsSequence()) {
        for (const auto &device_node : devices) {
            DeviceSpec spec;
            if (const auto id = sdkcfg::as_string(device_node["id"])) {
                spec.id = *id;
            }
            if (const auto type = sdkcfg::as_string(device_node["type"])) {
                spec.type = parse_device_type(*type);
            }
            // Dynamic default (label = id) is provider-side by design; the
            // schema documents it in the field description.
            spec.label = sdkcfg::as_string(device_node["label"]).value_or(spec.id);
            if (const auto address = sdkcfg::parse_i2c_address(device_node["address"])) {
                spec.address = *address;
            }
            if (const auto watchdog = sdkcfg::as_int64(device_node["command_watchdog_ms"])) {
                spec.command_watchdog_ms = static_cast<int>(*watchdog);
            }
            if (const auto encoders = sdkcfg::as_bool(device_node["encoders"])) {
                spec.encoders = *encoders;
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
        << ", provider.outputs_on_startup=" << to_string(config.outputs_on_startup)
        << ", hardware.bus_path=" << config.bus_path << ", hardware.query_delay_us=" << config.query_delay_us
        << ", hardware.timeout_ms=" << config.timeout_ms << ", hardware.retry_count=" << config.retry_count;
    if (config.max_bus_hz) {
        out << ", hardware.max_bus_hz=" << *config.max_bus_hz;
    }
    out << ", discovery.mode=" << to_string(config.discovery_mode) << ", devices=" << config.devices.size();

    if (config.discovery_mode == DiscoveryMode::Manual) {
        out << ", discovery.addresses=[";
        for (std::size_t i = 0; i < config.manual_addresses.size(); ++i) {
            if (i > 0) {
                out << ", ";
            }
            out << format_i2c_address(config.manual_addresses[i]);
        }
        out << "]";
    }

    return out.str();
}

}  // namespace anolis_provider_bread
