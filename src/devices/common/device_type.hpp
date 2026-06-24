#pragma once

/**
 * @file device_type.hpp
 * @brief The closed set of BREAD device families.
 *
 * Lives in the devices layer (not `config/`) so device-layer code can name the
 * taxonomy without depending on provider configuration. This keeps the
 * dependency edge `config → devices` (config parses into the taxonomy), the
 * direction the Wave-5 SDK extraction requires (`provider → SDK → contract`):
 * the forthcoming device-adapter descriptor must not depend on provider config.
 * The enum stays provider-local; only the descriptor + helpers become SDK-bound.
 */

namespace anolis_provider_bread {

/**
 * @brief Supported BREAD device families exposed by this provider.
 */
enum class DeviceType {
    Rlht,
    Dcmt,
};

}  // namespace anolis_provider_bread
