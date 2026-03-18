#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

#include "config/provider_config.hpp"
#include "core/handlers.hpp"
#include "core/runtime_state.hpp"
#include "core/transport/framed_stdio.hpp"
#include "protocol.pb.h"

namespace {

anolis_provider_bread::ProviderConfig make_stub_config() {
    anolis_provider_bread::ProviderConfig config;
    config.provider_name = "bread-lab";
    config.bus_path = "/dev/i2c-1";
    config.discovery_mode = anolis_provider_bread::DiscoveryMode::Manual;
    config.manual_addresses = {0x08, 0x09};
    config.devices = {
        anolis_provider_bread::DeviceSpec{"rlht0", anolis_provider_bread::DeviceType::Rlht, "Left Heater", 0x08},
        anolis_provider_bread::DeviceSpec{"dcmt0", anolis_provider_bread::DeviceType::Dcmt, "Conveyor Drive", 0x09},
    };
    return config;
}

} // namespace

TEST(ProtoSmokeTest, CanConstructHelloRequest) {
    anolis::deviceprovider::v1::HelloRequest request;
    request.set_protocol_version("v1");
    request.set_client_name("proto-smoke-test");

    EXPECT_EQ(request.protocol_version(), "v1");
    EXPECT_EQ(request.client_name(), "proto-smoke-test");
}

TEST(ProtoSmokeTest, CanPopulateAndSerializeHelloResponseManually) {
    anolis::deviceprovider::v1::Response response;
    response.set_request_id(1);
    response.mutable_status()->set_code(anolis::deviceprovider::v1::Status::CODE_OK);
    response.mutable_status()->set_message("ok");

    auto *hello = response.mutable_hello();
    hello->set_protocol_version("v1");
    hello->set_provider_name("anolis-provider-bread");
    hello->set_provider_version("0.1.0");
    (*hello->mutable_metadata())["transport"] = "stdio+uint32_le";
    (*hello->mutable_metadata())["inventory_mode"] = "config_seeded";

    std::string payload;
    ASSERT_TRUE(response.SerializeToString(&payload));
    EXPECT_FALSE(payload.empty());
}

TEST(ProtoSmokeTest, HelloHandlerProducesSerializableResponse) {
    anolis_provider_bread::runtime::reset();
    anolis_provider_bread::runtime::initialize(make_stub_config());

    anolis::deviceprovider::v1::HelloRequest request;
    request.set_protocol_version("v1");
    request.set_client_name("proto-smoke-test");
    request.set_client_version("0.1.0");

    anolis::deviceprovider::v1::Response response;
    response.set_request_id(1);

    anolis_provider_bread::handlers::handle_hello(request, response);

    EXPECT_EQ(response.status().code(), anolis::deviceprovider::v1::Status::CODE_OK);
    EXPECT_EQ(response.hello().provider_name(), "anolis-provider-bread");
    EXPECT_EQ(response.hello().metadata().at("inventory_mode"), "config_seeded");

    std::string payload;
    ASSERT_TRUE(response.SerializeToString(&payload));
    EXPECT_FALSE(payload.empty());
}

TEST(ProtoSmokeTest, HelloHandlerStillSerializesAfterRuntimeInitialization) {
    anolis_provider_bread::runtime::reset();
    anolis_provider_bread::runtime::initialize(make_stub_config());

    anolis::deviceprovider::v1::HelloRequest request;
    request.set_protocol_version("v1");
    request.set_client_name("proto-smoke-test");
    request.set_client_version("0.1.0");

    anolis::deviceprovider::v1::Response response;
    response.set_request_id(1);
    response.mutable_status()->set_code(anolis::deviceprovider::v1::Status::CODE_INTERNAL);
    response.mutable_status()->set_message("uninitialized");

    anolis_provider_bread::handlers::handle_hello(request, response);

    std::string payload;
    ASSERT_TRUE(response.SerializeToString(&payload));
    EXPECT_FALSE(payload.empty());
}

TEST(ProtoSmokeTest, DescribeDeviceHandlerProducesSerializableResponseAfterRuntimeInitialization) {
    anolis_provider_bread::runtime::reset();
    anolis_provider_bread::runtime::initialize(make_stub_config());

    anolis::deviceprovider::v1::DescribeDeviceRequest request;
    request.set_device_id("rlht0");

    anolis::deviceprovider::v1::Response response;
    response.set_request_id(7);
    response.mutable_status()->set_code(anolis::deviceprovider::v1::Status::CODE_INTERNAL);
    response.mutable_status()->set_message("uninitialized");

    anolis_provider_bread::handlers::handle_describe_device(request, response);

    EXPECT_EQ(response.status().code(), anolis::deviceprovider::v1::Status::CODE_OK);
    EXPECT_EQ(response.describe_device().device().type_id(), "bread.rlht");
    EXPECT_EQ(response.describe_device().capabilities().functions_size(), 6);

    std::string payload;
    ASSERT_TRUE(response.SerializeToString(&payload));
    EXPECT_FALSE(payload.empty());
}

TEST(ProtoSmokeTest, InMemoryProviderLoopCanRoundTripHello) {
    anolis_provider_bread::runtime::reset();
    anolis_provider_bread::runtime::initialize(make_stub_config());

    anolis::deviceprovider::v1::Request request;
    request.set_request_id(42);
    request.mutable_hello()->set_protocol_version("v1");
    request.mutable_hello()->set_client_name("proto-smoke-test");
    request.mutable_hello()->set_client_version("0.1.0");

    std::string request_payload;
    ASSERT_TRUE(request.SerializeToString(&request_payload));

    std::ostringstream outbound(std::ios::binary);
    std::string io_error;
    ASSERT_TRUE(anolis_provider_bread::transport::write_frame(
        outbound,
        reinterpret_cast<const uint8_t *>(request_payload.data()),
        request_payload.size(),
        io_error)) << io_error;

    std::string raw_frame = outbound.str();
    std::istringstream inbound(raw_frame, std::ios::binary);
    std::vector<uint8_t> frame;
    ASSERT_TRUE(anolis_provider_bread::transport::read_frame(inbound, frame, io_error)) << io_error;

    anolis::deviceprovider::v1::Request parsed_request;
    ASSERT_TRUE(parsed_request.ParseFromArray(frame.data(), static_cast<int>(frame.size())));

    anolis::deviceprovider::v1::Response response;
    response.set_request_id(parsed_request.request_id());
    response.mutable_status()->set_code(anolis::deviceprovider::v1::Status::CODE_INTERNAL);
    response.mutable_status()->set_message("uninitialized");
    anolis_provider_bread::handlers::handle_hello(parsed_request.hello(), response);

    std::string response_payload;
    ASSERT_TRUE(response.SerializeToString(&response_payload));

    std::ostringstream framed_response(std::ios::binary);
    ASSERT_TRUE(anolis_provider_bread::transport::write_frame(
        framed_response,
        reinterpret_cast<const uint8_t *>(response_payload.data()),
        response_payload.size(),
        io_error)) << io_error;
    EXPECT_FALSE(framed_response.str().empty());
}
