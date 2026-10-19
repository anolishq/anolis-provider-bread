#include "devices/common/bread_compatibility.hpp"

/**
 * @file bread_compatibility.cpp
 * @brief Version and type compatibility checks for the supported BREAD
 * families.
 */

#include <sstream>
#include <utility>

extern "C" {
#include <bread/bread_version_helpers.h>
#include <bread/dcmt_ops.h>
#include <bread/rlht_ops.h>
}

namespace anolis_provider_bread::inventory {
namespace {

std::pair<uint8_t, uint8_t> expected_module_version(uint8_t type_id) {
    switch (type_id) {
        // NOLINTNEXTLINE(bugprone-branch-clone): RLHT/DCMT are distinct device types.
        case RLHT_TYPE_ID:
            return {RLHT_MODULE_VER_MAJOR, RLHT_MODULE_VER_MINOR};
        case DCMT_TYPE_ID:
            return {DCMT_MODULE_VER_MAJOR, DCMT_MODULE_VER_MINOR};
        default:
            return {0u, 0u};
    }
}

}  // namespace

bool try_parse_bread_type(uint8_t type_id, DeviceType &out) {
    switch (type_id) {
        case RLHT_TYPE_ID:
            out = DeviceType::Rlht;
            return true;
        case DCMT_TYPE_ID:
            out = DeviceType::Dcmt;
            return true;
        default:
            return false;
    }
}

uint8_t bread_type_id(DeviceType type) {
    switch (type) {
        case DeviceType::Rlht:
            return RLHT_TYPE_ID;
        case DeviceType::Dcmt:
            return DCMT_TYPE_ID;
    }

    return 0u;
}

std::string bread_contract_name(DeviceType type) {
    switch (type) {
        case DeviceType::Rlht:
            return "rlht";
        case DeviceType::Dcmt:
            return "dcmt";
    }

    return "unknown";
}

std::string provider_type_id(DeviceType type) {
    switch (type) {
        case DeviceType::Rlht:
            return "bread.rlht";
        case DeviceType::Dcmt:
            return "bread.dcmt";
    }

    return "bread.unknown";
}

ProbeStatus evaluate_version_compatibility(uint8_t type_id, const ModuleVersion &version, std::string *detail) {
    if (detail) {
        detail->clear();
    }

    const auto expected = expected_module_version(type_id);
    if (expected.first == 0u) {
        if (detail) {
            *detail = "unsupported BREAD type id";
        }
        return ProbeStatus::UnsupportedType;
    }

    // CRUMBS compatibility is checked before the module ABI because an old bus
    // protocol version makes the module-specific expectations meaningless.
    if (bread_check_crumbs_compat(version.crumbs_version) != 0) {
        if (detail) {
            std::ostringstream out;
            out << "crumbs version " << version.crumbs_version << " is below minimum";
            *detail = out.str();
        }
        return ProbeStatus::IncompatibleCrumbsVersion;
    }

    return evaluate_module_version(version, expected.first, expected.second, detail);
}

ProbeStatus evaluate_module_version(const ModuleVersion &version, uint8_t expected_major, uint8_t expected_minor,
                                    std::string *detail) {
    if (detail) {
        detail->clear();
    }

    // The module major is the wire-compatibility number: the adapters assume
    // the payload layouts of the major this build was compiled against. Minor
    // and patch never gate; optional features follow the caps flags.
    if (bread_check_module_compat(version.module_major, version.module_minor, expected_major, expected_minor) != 0) {
        if (detail) {
            std::ostringstream out;
            out << "module major mismatch: expected " << static_cast<int>(expected_major) << " got "
                << static_cast<int>(version.module_major);
            *detail = out.str();
        }
        return ProbeStatus::IncompatibleModuleMajor;
    }

    if (detail) {
        if (version.module_minor < expected_minor) {
            std::ostringstream out;
            out << "compatible: module minor " << static_cast<int>(version.module_minor) << " is older than "
                << static_cast<int>(expected_minor) << ", optional features follow the caps flags";
            *detail = out.str();
        } else {
            *detail = "compatible";
        }
    }
    return ProbeStatus::Supported;
}

std::string to_string(ProbeStatus status) {
    switch (status) {
        case ProbeStatus::Supported:
            return "supported";
        case ProbeStatus::UnsupportedType:
            return "unsupported_type";
        case ProbeStatus::VersionReadFailed:
            return "version_read_failed";
        case ProbeStatus::IncompatibleCrumbsVersion:
            return "incompatible_crumbs_version";
        case ProbeStatus::IncompatibleModuleMajor:
            return "incompatible_module_major";
        case ProbeStatus::TypeMismatch:
            return "type_mismatch";
    }

    return "unknown";
}

}  // namespace anolis_provider_bread::inventory
