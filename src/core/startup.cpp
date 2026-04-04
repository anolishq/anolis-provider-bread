/**
 * @file startup.cpp
 * @brief Discovery flow for probing BREAD devices and assembling runtime inventory.
 *
 * Startup probes each candidate address independently. Unsupported,
 * incompatible, or missing devices are recorded as diagnostics rather than
 * aborting the whole inventory build unless the bus-wide scan itself fails.
 */

#include "core/startup.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "devices/common/bread_compatibility.hpp"
#include "logging/logger.hpp"

extern "C" {
#include <bread/bread_caps.h>
#include <bread/bread_version_helpers.h>
}

namespace anolis_provider_bread::startup {
namespace {

std::string hex_byte(uint8_t value) {
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex
        << std::setw(2) << std::setfill('0') << static_cast<int>(value);
    return out.str();
}

} // namespace

inventory::ProbeRecord probe_device(crumbs::Session &session, uint8_t address) {
    inventory::ProbeRecord probe;
    probe.address = static_cast<int>(address);

    // The probe sequence is intentionally staged: version read establishes the
    // family and module version, compatibility gating rejects unsupported
    // modules early, and the caps query refines the capability surface when it
    // succeeds.
    crumbs::RawFrame version_frame;
    const crumbs::SessionStatus version_status =
        session.query_read(address, 0x00, version_frame);

    if (!version_status) {
        probe.status = inventory::ProbeStatus::VersionReadFailed;
        probe.detail = "version query failed: " + version_status.message;
        logging::warning("probe " + format_i2c_address(static_cast<int>(address)) +
                         " version failed: " + version_status.message);
        return probe;
    }

    if (version_frame.opcode != 0x00) {
        probe.status = inventory::ProbeStatus::VersionReadFailed;
        probe.detail =
            "unexpected opcode in version reply: " + hex_byte(version_frame.opcode);
        logging::warning("probe " + format_i2c_address(static_cast<int>(address)) +
                         " unexpected version reply opcode " +
                         hex_byte(version_frame.opcode));
        return probe;
    }

    probe.type_id = version_frame.type_id;

    // Unknown BREAD families are excluded before any compatibility or caps work.
    DeviceType device_type{};
    if (!inventory::try_parse_bread_type(version_frame.type_id, device_type)) {
        probe.status = inventory::ProbeStatus::UnsupportedType;
        probe.detail = "unknown BREAD type_id: " + hex_byte(version_frame.type_id);
        logging::info("probe " + format_i2c_address(static_cast<int>(address)) +
                      " unsupported type_id " + hex_byte(version_frame.type_id));
        return probe;
    }

    if (version_frame.payload.size() < 5u) {
        probe.status = inventory::ProbeStatus::VersionReadFailed;
        probe.detail = "version payload too short (" +
                       std::to_string(version_frame.payload.size()) + " bytes)";
        logging::warning("probe " + format_i2c_address(static_cast<int>(address)) +
                         " version payload too short (" +
                         std::to_string(version_frame.payload.size()) + " bytes)");
        return probe;
    }

    uint16_t crumbs_ver = 0;
    uint8_t mod_major = 0;
    uint8_t mod_minor = 0;
    uint8_t mod_patch = 0;
    if (bread_parse_version(version_frame.payload.data(),
                            static_cast<uint8_t>(version_frame.payload.size()),
                            &crumbs_ver, &mod_major, &mod_minor, &mod_patch) != 0) {
        probe.status = inventory::ProbeStatus::VersionReadFailed;
        probe.detail = "failed to parse version payload";
        logging::warning("probe " + format_i2c_address(static_cast<int>(address)) +
                         " failed to parse version payload");
        return probe;
    }

    inventory::ModuleVersion version;
    version.crumbs_version = crumbs_ver;
    version.module_major   = mod_major;
    version.module_minor   = mod_minor;
    version.module_patch   = mod_patch;
    probe.version = version;

    std::string compat_detail;
    const inventory::ProbeStatus compat = inventory::evaluate_version_compatibility(
        version_frame.type_id, version, &compat_detail);
    if (compat != inventory::ProbeStatus::Supported) {
        probe.status = compat;
        probe.detail = compat_detail;
        logging::warning("probe " + format_i2c_address(static_cast<int>(address)) +
                         " compat failed: " + compat_detail);
        return probe;
    }

    // Capability discovery is best-effort. A failed caps read does not remove a
    // supported module from inventory; it falls back to the type baseline.
    crumbs::RawFrame caps_frame;
    const crumbs::SessionStatus caps_status =
        session.query_read(address, BREAD_OP_GET_CAPS, caps_frame);

    if (!caps_status || caps_frame.opcode != BREAD_OP_GET_CAPS) {
        probe.capability_profile =
            inventory::make_baseline_capability_profile(device_type);
        if (!caps_status) {
            logging::warning("probe " + format_i2c_address(static_cast<int>(address)) +
                             " caps query failed (" + caps_status.message +
                             "), using baseline fallback");
        } else {
            logging::warning("probe " + format_i2c_address(static_cast<int>(address)) +
                             " unexpected caps reply opcode " +
                             hex_byte(caps_frame.opcode) +
                             ", using baseline fallback");
        }
    } else {
        bread_caps_result_t caps_result{};
        if (bread_caps_parse_payload(caps_frame.payload.data(),
                                     static_cast<uint8_t>(caps_frame.payload.size()),
                                     &caps_result) == 0) {
            inventory::CapabilityProfile profile;
            profile.schema = caps_result.schema;
            profile.level  = caps_result.level;
            profile.flags  = caps_result.flags;
            profile.source = inventory::CapabilitySource::Queried;
            probe.capability_profile = profile;
        } else {
            probe.capability_profile =
                inventory::make_baseline_capability_profile(device_type);
            logging::warning("probe " + format_i2c_address(static_cast<int>(address)) +
                             " caps parse failed, using baseline fallback");
        }
    }

    probe.status = inventory::ProbeStatus::Supported;
    probe.detail = "probed";

    std::ostringstream diag;
    diag << "probe " << format_i2c_address(static_cast<int>(address))
         << " type=" << hex_byte(version_frame.type_id)
         << " crumbs=" << crumbs_ver
         << " module=" << static_cast<int>(mod_major) << "."
         << static_cast<int>(mod_minor) << "." << static_cast<int>(mod_patch)
         << " caps_source=" << inventory::to_string(probe.capability_profile.source);
    logging::info(diag.str());

    return probe;
}

DiscoveryResult run_discovery(crumbs::Session &session, const ProviderConfig &config) {
    std::vector<inventory::ProbeRecord> probes;
    inventory::InventorySource source = inventory::InventorySource::Discovered;

    if (config.discovery_mode == DiscoveryMode::Scan) {
        // Scan mode treats bus-wide scan failure as a startup blocker because
        // the provider cannot know which addresses should be probed.
        crumbs::ScanOptions scan_opts{};
        std::vector<crumbs::ScanResult> scan_results;
        const crumbs::SessionStatus scan_status = session.scan(scan_opts, scan_results);
        if (!scan_status) {
            throw std::runtime_error("CRUMBS bus scan failed: " + scan_status.message);
        }

        logging::info("scan found " + std::to_string(scan_results.size()) +
                      " CRUMBS device(s)");
        probes.reserve(scan_results.size());
        for (const auto &result : scan_results) {
            probes.push_back(probe_device(session, result.address));
        }
        source = inventory::InventorySource::Discovered;
    } else {
        // Manual mode limits probing to the configured address set and still
        // records per-address failures as unsupported or missing results.
        probes.reserve(config.manual_addresses.size());
        for (const int addr : config.manual_addresses) {
            probes.push_back(probe_device(session, static_cast<uint8_t>(addr)));
        }
        source = inventory::InventorySource::Manual;
    }

    const inventory::InventoryBuildResult build =
        inventory::build_inventory_from_probes(config, probes, source);

    if (!build.unsupported_probes.empty()) {
        logging::warning(std::to_string(build.unsupported_probes.size()) +
                         " unsupported or incompatible probe(s) excluded from inventory");
        for (const auto &p : build.unsupported_probes) {
            logging::warning("  excluded " + format_i2c_address(p.address) +
                             " status=" + inventory::to_string(p.status) +
                             " detail=" + p.detail);
        }
    }

    if (!build.missing_expected_ids.empty()) {
        logging::warning(
            std::to_string(build.missing_expected_ids.size()) +
            " expected device(s) not found in inventory");
        for (const auto &id : build.missing_expected_ids) {
            logging::warning("  missing expected device id=" + id);
        }
    }

    DiscoveryResult result;
    result.devices               = std::move(build.supported_devices);
    result.unsupported_probes    = std::move(build.unsupported_probes);
    result.missing_expected_ids  = std::move(build.missing_expected_ids);
    result.inventory_mode        = inventory::to_string(source);
    return result;
}

} // namespace anolis_provider_bread::startup
