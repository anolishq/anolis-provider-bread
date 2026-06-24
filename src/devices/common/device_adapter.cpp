#include "devices/common/device_adapter.hpp"

#include "devices/dcmt/dcmt_adapter.hpp"
#include "devices/rlht/rlht_adapter.hpp"

namespace anolis_provider_bread {
namespace {

using anolis::deviceprovider::v1::Status;

// Unreachable fallback for adapter_for(): the switch is exhaustive over the
// closed DeviceType under -Werror=switch, so a missing case is a compile error
// first. These keep control flow total and fail safe (UNIMPLEMENTED) if reached.
AdapterReadResult unknown_read_signals(crumbs::Session & /*session*/, const inventory::InventoryDevice & /*device*/,
                                       const std::vector<std::string> & /*signal_ids*/) {
    return {false, Status::CODE_UNIMPLEMENTED, "unsupported device type", {}};
}
AdapterCallResult unknown_build_frame(uint32_t /*function_id*/, const ValueMap & /*args*/,
                                      crumbs::RawFrame & /*frame*/) {
    return {false, Status::CODE_UNIMPLEMENTED, "unsupported device type"};
}
AdapterCallResult unknown_transmit(crumbs::Session & /*session*/, const inventory::InventoryDevice & /*device*/,
                                   const crumbs::RawFrame & /*frame*/) {
    return {false, Status::CODE_UNIMPLEMENTED, "unsupported device type"};
}

const DeviceAdapter kRlhtAdapter{rlht::read_signals, rlht::build_frame, rlht::transmit};
const DeviceAdapter kDcmtAdapter{dcmt::read_signals, dcmt::build_frame, dcmt::transmit};
const DeviceAdapter kUnknownAdapter{unknown_read_signals, unknown_build_frame, unknown_transmit};

}  // namespace

const DeviceAdapter &adapter_for(DeviceType type) {
    switch (type) {
        case DeviceType::Rlht:
            return kRlhtAdapter;
        case DeviceType::Dcmt:
            return kDcmtAdapter;
    }
    return kUnknownAdapter;
}

AdapterCallResult call(const DeviceAdapter &adapter, crumbs::Session *session,
                       const inventory::InventoryDevice &device, uint32_t function_id, const ValueMap &args) {
    // ADPP §8.3: validate + encode before touching hardware.
    crumbs::RawFrame frame;
    AdapterCallResult result = adapter.build_frame(function_id, args, frame);
    if (!result.ok) {
        return result;
    }
    if (session == nullptr) {
        return {false, Status::CODE_UNAVAILABLE,
                "no hardware session (provider not built with hardware support)"};
    }
    return adapter.transmit(*session, device, frame);
}

}  // namespace anolis_provider_bread
