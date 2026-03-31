#include "config/provider_config.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

namespace anolis_provider_bread {
namespace {

class TempConfigFile {
public:
    explicit TempConfigFile(const std::string &yaml_body) {
        static std::atomic<unsigned long long> counter{0ULL};
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto id = counter.fetch_add(1ULL, std::memory_order_relaxed);

        path_ = std::filesystem::temp_directory_path() /
                ("anolis_provider_bread_config_test_" + std::to_string(nonce) + "_" +
                 std::to_string(id) + ".yaml");

        std::ofstream out(path_);
        if(!out.is_open()) {
            throw std::runtime_error("failed to create temp config: " + path_.string());
        }
        out << yaml_body;
        out.flush();
        if(!out.good()) {
            throw std::runtime_error("failed to write temp config: " + path_.string());
        }
    }

    ~TempConfigFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    const std::filesystem::path &path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void expect_config_error(const std::string &yaml_body,
                         const std::string &expected_token) {
    const TempConfigFile config(yaml_body);
    try {
        (void)load_config(config.path().string());
        FAIL() << "Expected load_config() to fail";
    } catch(const std::runtime_error &e) {
        const std::string message = e.what();
        EXPECT_NE(message.find(expected_token), std::string::npos)
            << "expected token: " << expected_token
            << "\nactual message: " << message;
    }
}

} // namespace

TEST(ProviderConfigTest, AppliesDefaultsForOptionalHardwareFields) {
    const TempConfigFile config(R"(
provider:
  name: bread-lab
hardware:
  bus_path: /dev/i2c-1
discovery:
  mode: scan
)");

    const ProviderConfig parsed = load_config(config.path().string());
    EXPECT_EQ(parsed.provider_name, "bread-lab");
    EXPECT_EQ(parsed.bus_path, "/dev/i2c-1");
    EXPECT_FALSE(parsed.require_live_session);
    EXPECT_EQ(parsed.query_delay_us, 10000);
    EXPECT_EQ(parsed.timeout_ms, 100);
    EXPECT_EQ(parsed.retry_count, 2);
    EXPECT_EQ(parsed.discovery_mode, DiscoveryMode::Scan);
    EXPECT_TRUE(parsed.manual_addresses.empty());
    EXPECT_TRUE(parsed.devices.empty());
}

TEST(ProviderConfigTest, ParsesManualDiscoveryAddressesAndDevices) {
    const TempConfigFile config(R"(
provider:
  name: bread.lab-1
hardware:
  bus_path: /dev/i2c-1
  retry_count: 3
discovery:
  mode: manual
  addresses: [0x08, 9]
devices:
  - id: rlht0
    type: rlht
    label: Left Heater
    address: 0x08
  - id: dcmt0
    type: dcmt
    address: 9
)");

    const ProviderConfig parsed = load_config(config.path().string());
    ASSERT_EQ(parsed.manual_addresses.size(), 2U);
    ASSERT_EQ(parsed.devices.size(), 2U);
    EXPECT_EQ(parsed.devices[0].type, DeviceType::Rlht);
    EXPECT_EQ(parsed.devices[0].label, "Left Heater");
    EXPECT_EQ(parsed.devices[1].type, DeviceType::Dcmt);
    EXPECT_EQ(parsed.devices[1].label, "dcmt0");
    EXPECT_EQ(parsed.devices[1].address, 0x09);
}

TEST(ProviderConfigTest, ParsesRequireLiveSession) {
    const TempConfigFile config(R"(
hardware:
  bus_path: /dev/i2c-1
  require_live_session: true
discovery:
  mode: scan
)");

    const ProviderConfig parsed = load_config(config.path().string());
    EXPECT_TRUE(parsed.require_live_session);
}

TEST(ProviderConfigTest, RejectsMissingHardwareBusPath) {
    expect_config_error(R"(
hardware:
  timeout_ms: 25
discovery:
  mode: scan
)", "hardware.bus_path");
}

TEST(ProviderConfigTest, RejectsManualModeWithoutAddresses) {
    expect_config_error(R"(
hardware:
  bus_path: /dev/i2c-1
discovery:
  mode: manual
)", "discovery.addresses");
}

TEST(ProviderConfigTest, RejectsDuplicateManualAddresses) {
    expect_config_error(R"(
hardware:
  bus_path: /dev/i2c-1
discovery:
  mode: manual
  addresses: [0x08, 8]
)", "Duplicate discovery address");
}

TEST(ProviderConfigTest, RejectsUnknownRootKey) {
    expect_config_error(R"(
hardware:
  bus_path: /dev/i2c-1
discovery:
  mode: scan
unexpected: true
)", "Unknown root key");
}

TEST(ProviderConfigTest, RejectsInvalidRequireLiveSessionValue) {
    expect_config_error(R"(
hardware:
  bus_path: /dev/i2c-1
  require_live_session: maybe
discovery:
  mode: scan
)", "hardware.require_live_session");
}

TEST(ProviderConfigTest, RejectsUnknownDeviceType) {
    expect_config_error(R"(
hardware:
  bus_path: /dev/i2c-1
discovery:
  mode: scan
devices:
  - id: foo0
    type: fancy
    address: 0x08
)", "Invalid devices[].type");
}

TEST(ProviderConfigTest, RejectsDuplicateDeviceIds) {
    expect_config_error(R"(
hardware:
  bus_path: /dev/i2c-1
discovery:
  mode: scan
devices:
  - id: rlht0
    type: rlht
    address: 0x08
  - id: rlht0
    type: dcmt
    address: 0x09
)", "Duplicate devices[].id");
}

TEST(ProviderConfigTest, RejectsDuplicateDeviceAddresses) {
    expect_config_error(R"(
hardware:
  bus_path: /dev/i2c-1
discovery:
  mode: scan
devices:
  - id: rlht0
    type: rlht
    address: 0x08
  - id: dcmt0
    type: dcmt
    address: 8
)", "Duplicate devices[].address");
}

} // namespace anolis_provider_bread
