/**
 * @file handlers.cpp
 * @brief Implementation of the BREAD provider's ADPP request handlers.
 *
 * Handlers validate selector shape against the runtime inventory and then
 * delegate live reads/calls to the appropriate BREAD device adapter.
 */

#include "core/handlers.hpp"

#include <string>
#include <vector>

#include "core/health.hpp"
#include "core/runtime_state.hpp"
#include "core/transport/framed_stdio.hpp"
#include "devices/common/inventory.hpp"
#include "devices/dcmt/dcmt_adapter.hpp"
#include "devices/rlht/rlht_adapter.hpp"
#include "logging/logger.hpp"

namespace anolis_provider_bread::handlers {
namespace {

using CallRequest = anolis::deviceprovider::v1::CallRequest;
using DescribeDeviceRequest = anolis::deviceprovider::v1::DescribeDeviceRequest;
using GetHealthRequest = anolis::deviceprovider::v1::GetHealthRequest;
using HelloRequest = anolis::deviceprovider::v1::HelloRequest;
using ListDevicesRequest = anolis::deviceprovider::v1::ListDevicesRequest;
using ReadSignalsRequest = anolis::deviceprovider::v1::ReadSignalsRequest;
using Response = anolis::deviceprovider::v1::Response;
using Status = anolis::deviceprovider::v1::Status;
using WaitReadyRequest = anolis::deviceprovider::v1::WaitReadyRequest;

void set_status_ok(Response &response) {
    response.mutable_status()->set_code(Status::CODE_OK);
    response.mutable_status()->set_message("ok");
}

void set_status(Response &response, Status::Code code, const std::string &message) {
    response.mutable_status()->set_code(code);
    response.mutable_status()->set_message(message);
}

const inventory::InventoryDevice *require_device(const runtime::RuntimeState &state,
                                            const std::string &device_id,
                                            Response &response) {
    if(device_id.empty()) {
        set_status(response, Status::CODE_INVALID_ARGUMENT, "device_id is required");
        return nullptr;
    }

    const inventory::InventoryDevice *device = inventory::find_device(state.devices, device_id);
    if(!device) {
        set_status(response, Status::CODE_NOT_FOUND, "unknown device_id");
        return nullptr;
    }

    return device;
}

} // namespace

void handle_hello(const HelloRequest &request, Response &response) {
    if(request.protocol_version() != "v1") {
        set_status(response, Status::CODE_FAILED_PRECONDITION,
                   "unsupported protocol_version; expected v1");
        return;
    }

    auto *hello = response.mutable_hello();
    hello->set_protocol_version("v1");
    hello->set_provider_name("anolis-provider-bread");
    hello->set_provider_version(ANOLIS_PROVIDER_BREAD_VERSION);
    (*hello->mutable_metadata())["transport"] = "stdio+uint32_le";
    (*hello->mutable_metadata())["max_frame_bytes"] = std::to_string(transport::kMaxFrameBytes);
    (*hello->mutable_metadata())["supports_wait_ready"] = "true";
    (*hello->mutable_metadata())["inventory_mode"] = runtime::snapshot().inventory_mode;
    set_status_ok(response);
}

void handle_wait_ready(const WaitReadyRequest &, Response &response) {
    auto *out = response.mutable_wait_ready();
    health::populate_wait_ready(runtime::snapshot(), *out);
    set_status_ok(response);
}

void handle_list_devices(const ListDevicesRequest &request, Response &response) {
    const runtime::RuntimeState state = runtime::snapshot();
    auto *out = response.mutable_list_devices();
    for(const auto &device : state.devices) {
        *out->add_devices() = device.descriptor;
    }
    if(request.include_health()) {
        for(const auto &health_entry : health::make_device_health(state)) {
            *out->add_device_health() = health_entry;
        }
    }
    set_status_ok(response);
}

void handle_describe_device(const DescribeDeviceRequest &request, Response &response) {
    const runtime::RuntimeState state = runtime::snapshot();
    const inventory::InventoryDevice *device = require_device(state, request.device_id(), response);
    if(!device) {
        return;
    }

    auto *out = response.mutable_describe_device();
    *out->mutable_device() = device->descriptor;
    *out->mutable_capabilities() = device->capabilities;
    set_status_ok(response);
}

void handle_read_signals(const ReadSignalsRequest &request, Response &response) {
    const runtime::RuntimeState state = runtime::snapshot();
    const inventory::InventoryDevice *device = require_device(state, request.device_id(), response);
    if(!device) {
        return;
    }

    for(const auto &signal_id : request.signal_ids()) {
        if(!inventory::signal_exists(*device, signal_id)) {
            set_status(response, Status::CODE_NOT_FOUND,
                       "unknown signal_id '" + signal_id + "'");
            return;
        }
    }

    crumbs::Session *session_ptr = runtime::session();
    if(!session_ptr) {
        set_status(response, Status::CODE_UNAVAILABLE,
                   "no hardware session (provider not built with hardware support)");
        return;
    }

    // Inventory validation happens before adapter dispatch so the adapter layer
    // only sees signal IDs that are declared in the device capability surface.
    const std::vector<std::string> signal_ids(
        request.signal_ids().begin(), request.signal_ids().end());

    AdapterReadResult adapter_result;
    if(device->type == DeviceType::Rlht) {
        adapter_result = rlht::read_signals(*session_ptr, *device, signal_ids);
    } else if(device->type == DeviceType::Dcmt) {
        adapter_result = dcmt::read_signals(*session_ptr, *device, signal_ids);
    } else {
        set_status(response, Status::CODE_UNIMPLEMENTED, "unsupported device type");
        return;
    }

    if(!adapter_result.ok) {
        logging::warning("read_signals device='" + request.device_id() +
                         "' failed: " + adapter_result.error_message);
        set_status(response, adapter_result.error_code, adapter_result.error_message);
        return;
    }

    auto *out = response.mutable_read_signals();
    out->set_device_id(request.device_id());
    for(const auto &v : adapter_result.values) {
        *out->add_values() = v;
    }
    set_status_ok(response);
}

void handle_call(const CallRequest &request, Response &response) {
    const runtime::RuntimeState state = runtime::snapshot();
    const inventory::InventoryDevice *device = require_device(state, request.device_id(), response);
    if(!device) {
        return;
    }
    if(request.function_id() == 0 && request.function_name().empty()) {
        set_status(response, Status::CODE_INVALID_ARGUMENT,
                   "function_id or function_name is required");
        return;
    }
    if(!inventory::function_exists(*device, request.function_id(), request.function_name())) {
        set_status(response, Status::CODE_NOT_FOUND, "unknown function_id or function_name");
        return;
    }

    crumbs::Session *session_ptr = runtime::session();
    if(!session_ptr) {
        set_status(response, Status::CODE_UNAVAILABLE,
                   "no hardware session (provider not built with hardware support)");
        return;
    }

    // Name-based selectors resolve against the published capability surface so
    // handlers and external callers share the same function ID mapping.
    uint32_t fid = request.function_id();
    if(fid == 0) {
        for(const auto &fn : device->capabilities.functions()) {
            if(fn.name() == request.function_name()) {
                fid = fn.function_id();
                break;
            }
        }
    }

    AdapterCallResult adapter_result;
    if(device->type == DeviceType::Rlht) {
        adapter_result = rlht::call(*session_ptr, *device, fid, request.args());
    } else if(device->type == DeviceType::Dcmt) {
        adapter_result = dcmt::call(*session_ptr, *device, fid, request.args());
    } else {
        set_status(response, Status::CODE_UNIMPLEMENTED, "unsupported device type");
        return;
    }

    if(!adapter_result.ok) {
        const std::string fn_label = request.function_name().empty()
            ? std::to_string(request.function_id())
            : request.function_name();
        logging::warning("call device='" + request.device_id() +
                         "' fn='" + fn_label +
                         "' failed: " + adapter_result.error_message);
        set_status(response, adapter_result.error_code, adapter_result.error_message);
        return;
    }

    auto *out = response.mutable_call();
    out->set_device_id(request.device_id());
    set_status_ok(response);
}

void handle_get_health(const GetHealthRequest &, Response &response) {
    const runtime::RuntimeState state = runtime::snapshot();
    auto *out = response.mutable_get_health();
    *out->mutable_provider() = health::make_provider_health(state);
    for(const auto &device_health : health::make_device_health(state)) {
        *out->add_devices() = device_health;
    }
    set_status_ok(response);
}

void handle_unimplemented(Response &response, const std::string &message) {
    set_status(response, Status::CODE_UNIMPLEMENTED, message);
}

} // namespace anolis_provider_bread::handlers
