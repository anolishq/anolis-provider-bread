#pragma once

/**
 * @file bread_compatibility.hpp
 * @brief Type and version compatibility helpers for supported BREAD modules.
 */

#include <cstdint>
#include <string>

#include "config/provider_config.hpp"

namespace anolis_provider_bread::inventory {

/**
 * @brief Outcome of probing one BREAD address for provider compatibility.
 */
enum class ProbeStatus {
    Supported,
    UnsupportedType,
    VersionReadFailed,
    IncompatibleCrumbsVersion,
    IncompatibleModuleMajor,
    IncompatibleModuleMinor,
    TypeMismatch,
};

/**
 * @brief Version metadata reported by a probed BREAD module.
 */
struct ModuleVersion {
    uint16_t crumbs_version = 0;
    uint8_t module_major = 0;
    uint8_t module_minor = 0;
    uint8_t module_patch = 0;
};

/** @brief Convert a raw BREAD type identifier into a supported provider device type. */
bool try_parse_bread_type(uint8_t type_id, DeviceType &out);

/** @brief Return the canonical BREAD type identifier for a supported provider device type. */
uint8_t bread_type_id(DeviceType type);

/** @brief Return the short contract family name used in capability and probe reporting. */
std::string bread_contract_name(DeviceType type);

/** @brief Return the provider-level type identifier published in device descriptors. */
std::string provider_type_id(DeviceType type);

/**
 * @brief Evaluate whether a probed module version is supported by this provider build.
 *
 * Error handling:
 * Returns a specific incompatibility status instead of throwing. When `detail`
 * is non-null it receives a human-readable explanation suitable for logs or
 * probe diagnostics.
 */
ProbeStatus evaluate_version_compatibility(uint8_t type_id,
                                           const ModuleVersion &version,
                                           std::string *detail = nullptr);

/** @brief Convert a probe status enum to its stable diagnostic string form. */
std::string to_string(ProbeStatus status);

} // namespace anolis_provider_bread::inventory
