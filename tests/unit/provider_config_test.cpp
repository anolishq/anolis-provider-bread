#include "config/provider_config.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "config/config_schema.hpp"

namespace anolis_provider_bread {
namespace {

class TempConfigFile {
public:
    explicit TempConfigFile(const std::string &yaml_body) {
        static std::atomic<unsigned long long> counter{0ULL};
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto id = counter.fetch_add(1ULL, std::memory_order_relaxed);

        path_ = std::filesystem::temp_directory_path() /
                ("anolis_provider_bread_config_test_" + std::to_string(nonce) + "_" + std::to_string(id) + ".yaml");

        std::ofstream out(path_);
        if (!out.is_open()) {
            throw std::runtime_error("failed to create temp config: " + path_.string());
        }
        out << yaml_body;
        out.flush();
        if (!out.good()) {
            throw std::runtime_error("failed to write temp config: " + path_.string());
        }
    }

    ~TempConfigFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    const std::filesystem::path &path() const { return path_; }

private:
    std::filesystem::path path_;
};

void expect_config_error(const std::string &yaml_body, const std::string &expected_token) {
    const TempConfigFile config(yaml_body);
    try {
        (void)load_config(config.path().string());
        FAIL() << "Expected load_config() to fail";
    } catch (const std::runtime_error &e) {
        const std::string message = e.what();
        EXPECT_NE(message.find(expected_token), std::string::npos)
            << "expected token: " << expected_token << "\nactual message: " << message;
    }
}

}  // namespace

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
    EXPECT_FALSE(parsed.max_bus_hz.has_value());  // unset: no bus-clock host check
    EXPECT_EQ(parsed.discovery_mode, DiscoveryMode::Scan);
    EXPECT_TRUE(parsed.manual_addresses.empty());
    EXPECT_TRUE(parsed.devices.empty());
}

TEST(ProviderConfigTest, ParsesMaxBusHz) {
    const TempConfigFile config(R"(
hardware:
  bus_path: /dev/i2c-1
  max_bus_hz: 50000
discovery:
  mode: scan
)");

    const ProviderConfig parsed = load_config(config.path().string());
    ASSERT_TRUE(parsed.max_bus_hz.has_value());
    EXPECT_EQ(*parsed.max_bus_hz, 50000U);
    EXPECT_NE(summarize_config(parsed).find("hardware.max_bus_hz=50000"), std::string::npos);
}

TEST(ProviderConfigTest, RejectsNonPositiveMaxBusHz) {
    expect_config_error(R"(
hardware:
  bus_path: /dev/i2c-1
  max_bus_hz: 0
discovery:
  mode: scan
)",
                        "max_bus_hz");
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
    // command_watchdog_ms defaults to 0 (never arm) when absent.
    EXPECT_EQ(parsed.devices[0].command_watchdog_ms, 0);
    EXPECT_EQ(parsed.devices[1].command_watchdog_ms, 0);
}

TEST(ProviderConfigTest, ParsesDeviceCommandWatchdogMs) {
    const TempConfigFile config(R"(
hardware:
  bus_path: /dev/i2c-1
discovery:
  mode: scan
devices:
  - id: dcmt0
    type: dcmt
    address: 0x14
    command_watchdog_ms: 5000
)");

    const ProviderConfig parsed = load_config(config.path().string());
    ASSERT_EQ(parsed.devices.size(), 1U);
    EXPECT_EQ(parsed.devices[0].command_watchdog_ms, 5000);
}

TEST(ProviderConfigTest, RejectsCommandWatchdogMsBeyondU16) {
    expect_config_error(R"(
hardware:
  bus_path: /dev/i2c-1
discovery:
  mode: scan
devices:
  - id: dcmt0
    type: dcmt
    address: 0x14
    command_watchdog_ms: 70000
)",
                        "command_watchdog_ms");
}

TEST(ProviderConfigTest, RejectsMissingHardwareBusPath) {
    expect_config_error(R"(
hardware:
  timeout_ms: 25
discovery:
  mode: scan
)",
                        "hardware.bus_path");
}

TEST(ProviderConfigTest, RejectsManualModeWithoutAddresses) {
    expect_config_error(R"(
hardware:
  bus_path: /dev/i2c-1
discovery:
  mode: manual
)",
                        "discovery.addresses");
}

TEST(ProviderConfigTest, SchemaDefaultsAgreeWithBinaryDefaults) {
    // The schema's advertised defaults must be what the binary actually does
    // when the key is absent — a workbench form is rendered from the schema.
    const TempConfigFile config(R"(
hardware:
  bus_path: /dev/i2c-1
discovery:
  mode: scan
)");
    const ProviderConfig parsed = load_config(config.path().string());
    const ProviderConfig defaults;
    EXPECT_EQ(parsed.provider_name, defaults.provider_name);
    EXPECT_EQ(parsed.query_delay_us, defaults.query_delay_us);
    EXPECT_EQ(parsed.timeout_ms, defaults.timeout_ms);
    EXPECT_EQ(parsed.retry_count, defaults.retry_count);

    // ...and the declared schema carries those same values as its defaults.
    // Found-flags keep the loop from passing vacuously if a key is renamed.
    bool checked_name = false;
    bool checked_hardware = false;
    bool checked_watchdog = false;
    const auto &root_spec = provider_schema().root().spec();
    for (const auto &member : root_spec.members) {
        if (member.key == "provider") {
            for (const auto &field : member.object->spec().members) {
                if (field.key == "name") {
                    ASSERT_TRUE(field.field->spec().default_string.has_value());
                    EXPECT_EQ(*field.field->spec().default_string, defaults.provider_name);
                    checked_name = true;
                }
            }
        }
        if (member.key == "hardware") {
            for (const auto &field : member.object->spec().members) {
                if (!field.field.has_value()) {
                    continue;
                }
                const auto &spec = field.field->spec();
                if (field.key == "query_delay_us") {
                    EXPECT_EQ(spec.default_int, defaults.query_delay_us);
                }
                if (field.key == "timeout_ms") {
                    EXPECT_EQ(spec.default_int, defaults.timeout_ms);
                }
                if (field.key == "retry_count") {
                    EXPECT_EQ(spec.default_int, defaults.retry_count);
                    checked_hardware = true;
                }
            }
        }
        if (member.key == "devices") {
            for (const auto &field : member.array->spec().item_object->spec().members) {
                if (field.key == "command_watchdog_ms" && field.field.has_value()) {
                    EXPECT_EQ(field.field->spec().default_int, DeviceSpec{}.command_watchdog_ms);
                    checked_watchdog = true;
                }
            }
        }
    }
    EXPECT_TRUE(checked_name);
    EXPECT_TRUE(checked_hardware);
    EXPECT_TRUE(checked_watchdog);
}

TEST(ProviderConfigTest, RejectsAddressesUnderScanMode) {
    expect_config_error(R"(
hardware:
  bus_path: /dev/i2c-1
discovery:
  mode: scan
  addresses: [0x08]
)",
                        "not valid when mode is 'scan'");
}

TEST(ProviderConfigTest, RejectsEmptyManualAddressList) {
    expect_config_error(R"(
hardware:
  bus_path: /dev/i2c-1
discovery:
  mode: manual
  addresses: []
)",
                        "at least 1 item");
}

TEST(ProviderConfigTest, RejectsDuplicateManualAddresses) {
    expect_config_error(R"(
hardware:
  bus_path: /dev/i2c-1
discovery:
  mode: manual
  addresses: [0x08, 8]
)",
                        "duplicate value");
}

TEST(ProviderConfigTest, RejectsUnknownRootKey) {
    expect_config_error(R"(
hardware:
  bus_path: /dev/i2c-1
discovery:
  mode: scan
unexpected: true
)",
                        "unknown key");
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
)",
                        "invalid value");
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
)",
                        "duplicate id");
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
)",
                        "duplicate address");
}

}  // namespace anolis_provider_bread
