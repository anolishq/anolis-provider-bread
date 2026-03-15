#include <gtest/gtest.h>

#include "protocol.pb.h"

TEST(ProtoSmokeTest, CanConstructHelloRequest) {
    anolis::deviceprovider::v1::HelloRequest request;
    request.set_protocol_version("v1");
    request.set_client_name("phase0-test");

    EXPECT_EQ(request.protocol_version(), "v1");
    EXPECT_EQ(request.client_name(), "phase0-test");
}
