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
    EXPECT_EQ(parsed.query_delay_us, 10000);
    EXPECT_EQ(parsed.timeout_ms, 100);
    EXPECT_EQ(parsed.retry_count, 2);
    EXPECT_EQ(parsed.discovery_mode, DiscoveryMode::Scan);
    EXPECT_TRUE(parsed.manual_addresses.empty());
}

TEST(ProviderConfigTest, ParsesManualDiscoveryAddresses) {
    const TempConfigFile config(R"(
hardware:
  bus_path: /dev/i2c-1
  retry_count: 3
discovery:
  mode: manual
  addresses: [0x08, 9]
)");

    const ProviderConfig parsed = load_config(config.path().string());
    ASSERT_EQ(parsed.manual_addresses.size(), 2U);
    EXPECT_EQ(parsed.manual_addresses[0], 0x08);
    EXPECT_EQ(parsed.manual_addresses[1], 0x09);
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

} // namespace anolis_provider_bread
