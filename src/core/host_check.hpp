#pragma once

/**
 * @file host_check.hpp
 * @brief What a config needs from the host (executable profile v1 §6).
 *
 * Shared by `--check-host` and startup, so both report the same requirements.
 * A mock bus needs nothing; a real bus gets the SDK's i2c-dev checks, with the
 * clock compared against `hardware.max_bus_hz` when the config sets one.
 */

#include <string>
#include <vector>

#include "anolis/provider_sdk/host_check.hpp"
#include "config/provider_config.hpp"

namespace anolis_provider_bread {

std::vector<anolis::provider_sdk::host_check::Requirement> check_host(const ProviderConfig &config);

/** @brief "id: detail; id: detail" for the unmet requirements, empty if none. */
std::string summarize_unmet(const std::vector<anolis::provider_sdk::host_check::Requirement> &requirements);

}  // namespace anolis_provider_bread
