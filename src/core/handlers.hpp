#pragma once

/**
 * @file handlers.hpp
 * @brief ADPP request handlers for the BREAD provider.
 */

#include "protocol.pb.h"

namespace anolis_provider_bread::handlers {

/** @brief Handle the ADPP `Hello` handshake and advertise provider metadata. */
void handle_hello(const anolis::deviceprovider::v1::HelloRequest &request,
                  anolis::deviceprovider::v1::Response &response);

/** @brief Report provider readiness and startup diagnostics. */
void handle_wait_ready(const anolis::deviceprovider::v1::WaitReadyRequest &request,
                       anolis::deviceprovider::v1::Response &response);

/** @brief List active devices and optional device-health summaries. */
void handle_list_devices(const anolis::deviceprovider::v1::ListDevicesRequest &request,
                         anolis::deviceprovider::v1::Response &response);

/** @brief Describe one inventory device and its capability surface. */
void handle_describe_device(const anolis::deviceprovider::v1::DescribeDeviceRequest &request,
                            anolis::deviceprovider::v1::Response &response);

/** @brief Read one device's signals through the matching BREAD adapter. */
void handle_read_signals(const anolis::deviceprovider::v1::ReadSignalsRequest &request,
                         anolis::deviceprovider::v1::Response &response);

/** @brief Execute one device function through the matching BREAD adapter. */
void handle_call(const anolis::deviceprovider::v1::CallRequest &request,
                 anolis::deviceprovider::v1::Response &response);

/** @brief Return provider and device health derived from runtime state. */
void handle_get_health(const anolis::deviceprovider::v1::GetHealthRequest &request,
                       anolis::deviceprovider::v1::Response &response);

/** @brief Return the standard unimplemented response for unsupported operations. */
void handle_unimplemented(anolis::deviceprovider::v1::Response &response,
                          const std::string &message);

} // namespace anolis_provider_bread::handlers
