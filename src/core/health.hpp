#pragma once

/**
 * @file health.hpp
 * @brief Health projection helpers for the BREAD provider runtime snapshot.
 */

#include <string>
#include <vector>

#include "core/runtime_state.hpp"
#include "protocol.pb.h"

namespace anolis_provider_bread::health {

using DeviceHealth = anolis::deviceprovider::v1::DeviceHealth;
using ProviderHealth = anolis::deviceprovider::v1::ProviderHealth;
using WaitReadyResponse = anolis::deviceprovider::v1::WaitReadyResponse;

/**
 * @brief Build provider-level health from startup readiness and discovery diagnostics.
 */
ProviderHealth make_provider_health(const runtime::RuntimeState &state);

/**
 * @brief Build per-device health from the current inventory and missing-expected list.
 */
std::vector<DeviceHealth> make_device_health(const runtime::RuntimeState &state);

/**
 * @brief Populate the ADPP WaitReady response with startup diagnostics.
 */
void populate_wait_ready(const runtime::RuntimeState &state, WaitReadyResponse &response);

} // namespace anolis_provider_bread::health
