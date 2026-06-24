#pragma once

/**
 * @file device_adapter.hpp
 * @brief Uniform per-type device-adapter descriptor + the single typed dispatch
 * point for BREAD device types.
 *
 * Each device type (rlht/dcmt) exposes its behavior through this fn-ptr
 * descriptor; `adapter_for()` is the one place device-type dispatch happens — an
 * exhaustive switch over the closed `DeviceType` (compiler-checked under
 * `-Werror=switch`), not a registry. Descriptors are `static const`, stateless.
 *
 * `build_frame`/`transmit` are BREAD's two-stage CRUMBS implementation of a
 * function call (ADPP §8.3: validate+encode before touching hardware); they sit
 * behind the `call()` surface below rather than being a public axis.
 */

#include <cstdint>
#include <string>
#include <vector>

#include "crumbs/session.hpp"
#include "devices/common/adapter_helpers.hpp"
#include "devices/common/device_type.hpp"
#include "devices/common/inventory.hpp"

namespace anolis_provider_bread {

struct DeviceAdapter {
    AdapterReadResult (*read_signals)(crumbs::Session &session, const inventory::InventoryDevice &device,
                                      const std::vector<std::string> &signal_ids);
    AdapterCallResult (*build_frame)(uint32_t function_id, const ValueMap &args, crumbs::RawFrame &frame);
    AdapterCallResult (*transmit)(crumbs::Session &session, const inventory::InventoryDevice &device,
                                  const crumbs::RawFrame &frame);
};

/** @brief The single dispatch site — exhaustive over DeviceType. */
const DeviceAdapter &adapter_for(DeviceType type);

/**
 * @brief Encode then (if hardware is available) transmit one function call.
 *
 * ADPP §8.3: `build_frame` validates and encodes the request WITHOUT touching
 * hardware, so argument errors surface (INVALID_ARGUMENT / OUT_OF_RANGE) even
 * when no session is available; `transmit` runs only once a session is present.
 * `session` may be null (provider built without hardware support). Composes the
 * descriptor's two-stage primitives so the request handler stays device-agnostic.
 */
AdapterCallResult call(const DeviceAdapter &adapter, crumbs::Session *session,
                       const inventory::InventoryDevice &device, uint32_t function_id, const ValueMap &args);

}  // namespace anolis_provider_bread
