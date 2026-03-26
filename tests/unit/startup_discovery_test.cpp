#include "core/startup.hpp"

#include <map>

#include <gtest/gtest.h>

#include "config/provider_config.hpp"
#include "crumbs/session.hpp"
#include "devices/common/bread_compatibility.hpp"
#include "devices/common/inventory.hpp"

extern "C" {
#include <bread/bread_caps.h>
#include <bread/dcmt_ops.h>
#include <bread/rlht_ops.h>
#include <crumbs_version.h>
}

namespace anolis_provider_bread::startup {
namespace {

// ---------------------------------------------------------------------------
// ScriptedTransport: drives probe_device / run_discovery without hardware.
//
// Usage:
//   transport.scan_results = { {address, type_id, /*has_type_id=*/true} };
//   transport.add_reply(address, opcode, type_id, payload);
//   transport.add_read_error(address, opcode, SessionErrorCode::ReadFailed);
// ---------------------------------------------------------------------------
struct ScriptedReply {
    uint8_t type_id = 0;
    uint8_t opcode = 0;
    std::vector<uint8_t> payload;
};

class ScriptedTransport final : public crumbs::Transport {
public:
    std::vector<crumbs::ScanResult> scan_results;

    // Scripted happy-path replies: replies[address][opcode]
    std::map<uint8_t, std::map<uint8_t, ScriptedReply>> replies;
    // Scripted failures: read_errors[address][opcode]
    std::map<uint8_t, std::map<uint8_t, crumbs::SessionErrorCode>> read_errors;

    void add_reply(uint8_t address, uint8_t opcode, uint8_t type_id,
                   std::vector<uint8_t> payload) {
        replies[address][opcode] = {type_id, opcode, std::move(payload)};
    }

    void add_read_error(uint8_t address, uint8_t opcode, crumbs::SessionErrorCode code) {
        read_errors[address][opcode] = code;
    }

    // --- Transport interface -------------------------------------------------

    crumbs::SessionStatus open(const crumbs::SessionOptions &) override {
        open_ = true;
        return crumbs::SessionStatus::success();
    }

    void close() noexcept override { open_ = false; }
    bool is_open() const override { return open_; }
    void delay_us(uint32_t) override {}

    crumbs::SessionStatus scan(const crumbs::ScanOptions &,
                               std::vector<crumbs::ScanResult> &out) override {
        out = scan_results;
        return crumbs::SessionStatus::success();
    }

    crumbs::SessionStatus send(uint8_t address,
                               const crumbs::RawFrame &frame) override {
        // Record the target opcode from SET_REPLY frames so read() can respond.
        if (frame.opcode == crumbs::kSetReplyOpcode && !frame.payload.empty()) {
            pending_address_ = address;
            pending_opcode_  = frame.payload[0];
        }
        return crumbs::SessionStatus::success();
    }

    crumbs::SessionStatus read(uint8_t address, crumbs::RawFrame &frame,
                               uint32_t /*timeout_us*/) override {
        // Check for a scripted failure first.
        auto err_it = read_errors.find(address);
        if (err_it != read_errors.end()) {
            auto opc_it = err_it->second.find(pending_opcode_);
            if (opc_it != err_it->second.end()) {
                return crumbs::SessionStatus::failure(opc_it->second,
                                                      "scripted read error");
            }
        }

        // Return the scripted happy-path reply.
        auto addr_it = replies.find(address);
        if (addr_it == replies.end()) {
            return crumbs::SessionStatus::failure(
                crumbs::SessionErrorCode::ReadFailed, "no script for address");
        }
        auto opc_it = addr_it->second.find(pending_opcode_);
        if (opc_it == addr_it->second.end()) {
            return crumbs::SessionStatus::failure(
                crumbs::SessionErrorCode::ReadFailed, "no script for opcode");
        }

        frame.type_id  = opc_it->second.type_id;
        frame.opcode   = opc_it->second.opcode;
        frame.payload  = opc_it->second.payload;
        return crumbs::SessionStatus::success();
    }

private:
    bool    open_           = false;
    uint8_t pending_address_ = 0;
    uint8_t pending_opcode_  = 0;
};

// ---------------------------------------------------------------------------
// Payload builders
// ---------------------------------------------------------------------------

// [crumbs_ver:u16_le][mod_major:u8][mod_minor:u8][mod_patch:u8]
std::vector<uint8_t> make_version_payload(uint16_t crumbs_ver,
                                          uint8_t major,
                                          uint8_t minor,
                                          uint8_t patch) {
    return {
        static_cast<uint8_t>(crumbs_ver & 0xFF),
        static_cast<uint8_t>((crumbs_ver >> 8) & 0xFF),
        major, minor, patch
    };
}

// [schema:u8][level:u8][flags:u32_le]
std::vector<uint8_t> make_caps_payload(uint8_t schema, uint8_t level, uint32_t flags) {
    return {
        schema, level,
        static_cast<uint8_t>( flags        & 0xFF),
        static_cast<uint8_t>((flags >>  8) & 0xFF),
        static_cast<uint8_t>((flags >> 16) & 0xFF),
        static_cast<uint8_t>((flags >> 24) & 0xFF)
    };
}

// ---------------------------------------------------------------------------
// Test fixture helpers — build a minimal ProviderConfig
// ---------------------------------------------------------------------------

ProviderConfig make_scan_config() {
    ProviderConfig cfg;
    cfg.bus_path       = "/dev/i2c-test";
    cfg.discovery_mode = DiscoveryMode::Scan;
    cfg.query_delay_us = 0;
    return cfg;
}

ProviderConfig make_manual_config(std::vector<int> addresses,
                                  std::vector<DeviceSpec> devices = {}) {
    ProviderConfig cfg;
    cfg.bus_path          = "/dev/i2c-test";
    cfg.discovery_mode    = DiscoveryMode::Manual;
    cfg.manual_addresses  = std::move(addresses);
    cfg.devices           = std::move(devices);
    cfg.query_delay_us    = 0;
    return cfg;
}

// Open the session backed by the scripted transport.
std::unique_ptr<crumbs::Session> make_session(crumbs::Transport &transport) {
    crumbs::SessionOptions opts;
    opts.bus_path    = "/dev/i2c-test";
    opts.retry_count = 0;
    opts.timeout_ms  = 0;
    auto session = std::make_unique<crumbs::Session>(transport, opts);
    session->open();
    return session;
}

} // namespace

// ===========================================================================
// Tests
// ===========================================================================

// ---------------------------------------------------------------------------
// 1. Scan mode: single supported RLHT device with queried caps
// ---------------------------------------------------------------------------
TEST(StartupDiscoveryTest, ScanModeBuildInventoryFromSupportedRlhtDevice) {
    ScriptedTransport transport;

    crumbs::ScanResult scan_entry;
    scan_entry.address    = 0x0A;
    scan_entry.has_type_id = true;
    scan_entry.type_id    = RLHT_TYPE_ID;
    transport.scan_results = {scan_entry};

    transport.add_reply(0x0A, 0x00, RLHT_TYPE_ID,
                        make_version_payload(CRUMBS_VERSION,
                                             RLHT_MODULE_VER_MAJOR,
                                             RLHT_MODULE_VER_MINOR,
                                             RLHT_MODULE_VER_PATCH));
    transport.add_reply(0x0A, BREAD_OP_GET_CAPS, RLHT_TYPE_ID,
                        make_caps_payload(BREAD_CAPS_SCHEMA_V1,
                                          RLHT_CAP_LEVEL_1,
                                          RLHT_CAP_BASELINE_FLAGS));

    auto session = make_session(transport);
    const DiscoveryResult result = run_discovery(*session, make_scan_config());

    ASSERT_EQ(result.devices.size(), 1u);
    EXPECT_EQ(result.devices[0].address, 0x0A);
    EXPECT_EQ(result.devices[0].type, DeviceType::Rlht);
    EXPECT_EQ(result.devices[0].capability_profile.source,
              inventory::CapabilitySource::Queried);
    EXPECT_TRUE(result.unsupported_probes.empty());
    EXPECT_TRUE(result.missing_expected_ids.empty());
    EXPECT_EQ(result.inventory_mode, "discovered");
}

// ---------------------------------------------------------------------------
// 2. Manual mode: caps query fails → baseline fallback, device_id from config
// ---------------------------------------------------------------------------
TEST(StartupDiscoveryTest, ManualModeUsesBaselineFallbackOnCapsFailure) {
    ScriptedTransport transport;

    transport.add_reply(0x0A, 0x00, RLHT_TYPE_ID,
                        make_version_payload(CRUMBS_VERSION,
                                             RLHT_MODULE_VER_MAJOR,
                                             RLHT_MODULE_VER_MINOR,
                                             RLHT_MODULE_VER_PATCH));
    transport.add_read_error(0x0A, BREAD_OP_GET_CAPS,
                             crumbs::SessionErrorCode::ReadFailed);

    DeviceSpec spec;
    spec.id      = "rlht0";
    spec.type    = DeviceType::Rlht;
    spec.address = 0x0A;
    auto config = make_manual_config({0x0A}, {spec});

    auto session = make_session(transport);
    const DiscoveryResult result = run_discovery(*session, config);

    ASSERT_EQ(result.devices.size(), 1u);
    EXPECT_EQ(result.devices[0].descriptor.device_id(), "rlht0");
    EXPECT_EQ(result.devices[0].capability_profile.source,
              inventory::CapabilitySource::BaselineFallback);
    EXPECT_TRUE(result.missing_expected_ids.empty());
    EXPECT_EQ(result.inventory_mode, "manual");
}

// ---------------------------------------------------------------------------
// 3. Incompatible CRUMBS version → device excluded, recorded as unsupported
// ---------------------------------------------------------------------------
TEST(StartupDiscoveryTest, IncompatibleCrumbsVersionIsUnsupported) {
    ScriptedTransport transport;

    crumbs::ScanResult scan_entry;
    scan_entry.address     = 0x0A;
    scan_entry.has_type_id = true;
    scan_entry.type_id     = RLHT_TYPE_ID;
    transport.scan_results = {scan_entry};

    // Use a CRUMBS version below BREAD_MIN_CRUMBS_VERSION (1200).
    transport.add_reply(0x0A, 0x00, RLHT_TYPE_ID,
                        make_version_payload(1000u, 1, 0, 0));

    auto session = make_session(transport);
    const DiscoveryResult result = run_discovery(*session, make_scan_config());

    EXPECT_TRUE(result.devices.empty());
    ASSERT_EQ(result.unsupported_probes.size(), 1u);
    EXPECT_EQ(result.unsupported_probes[0].status,
              inventory::ProbeStatus::IncompatibleCrumbsVersion);
    EXPECT_EQ(result.unsupported_probes[0].address, 0x0A);
}

// ---------------------------------------------------------------------------
// 4. Manual mode: one device doesn't respond → tracked in missing_expected_ids
// ---------------------------------------------------------------------------
TEST(StartupDiscoveryTest, ManualModeTracksUnrespondingExpectedDevice) {
    ScriptedTransport transport;

    // 0x0A responds normally; 0x0B does not.
    transport.add_reply(0x0A, 0x00, RLHT_TYPE_ID,
                        make_version_payload(CRUMBS_VERSION,
                                             RLHT_MODULE_VER_MAJOR,
                                             RLHT_MODULE_VER_MINOR,
                                             RLHT_MODULE_VER_PATCH));
    transport.add_reply(0x0A, BREAD_OP_GET_CAPS, RLHT_TYPE_ID,
                        make_caps_payload(BREAD_CAPS_SCHEMA_V1,
                                          RLHT_CAP_LEVEL_1,
                                          RLHT_CAP_BASELINE_FLAGS));
    transport.add_read_error(0x0B, 0x00, crumbs::SessionErrorCode::ReadFailed);

    DeviceSpec spec0;
    spec0.id      = "rlht0";
    spec0.type    = DeviceType::Rlht;
    spec0.address = 0x0A;

    DeviceSpec spec1;
    spec1.id      = "rlht1";
    spec1.type    = DeviceType::Rlht;
    spec1.address = 0x0B;

    auto config = make_manual_config({0x0A, 0x0B}, {spec0, spec1});

    auto session = make_session(transport);
    const DiscoveryResult result = run_discovery(*session, config);

    ASSERT_EQ(result.devices.size(), 1u);
    EXPECT_EQ(result.devices[0].descriptor.device_id(), "rlht0");

    ASSERT_EQ(result.missing_expected_ids.size(), 1u);
    EXPECT_EQ(result.missing_expected_ids[0], "rlht1");
}

// ---------------------------------------------------------------------------
// 5. Scan mode: DCMT device with queried level-3 caps
// ---------------------------------------------------------------------------
TEST(StartupDiscoveryTest, ScanModeDcmtDeviceWithQueriedCaps) {
    ScriptedTransport transport;

    crumbs::ScanResult scan_entry;
    scan_entry.address     = 0x15;
    scan_entry.has_type_id = true;
    scan_entry.type_id     = DCMT_TYPE_ID;
    transport.scan_results = {scan_entry};

    transport.add_reply(0x15, 0x00, DCMT_TYPE_ID,
                        make_version_payload(CRUMBS_VERSION,
                                             DCMT_MODULE_VER_MAJOR,
                                             DCMT_MODULE_VER_MINOR,
                                             DCMT_MODULE_VER_PATCH));

    const uint32_t level3_flags = DCMT_CAP_OPEN_LOOP_CONTROL | DCMT_CAP_BRAKE_CONTROL |
                                  DCMT_CAP_CLOSED_LOOP_POSITION |
                                  DCMT_CAP_CLOSED_LOOP_SPEED | DCMT_CAP_PID_TUNING;
    transport.add_reply(0x15, BREAD_OP_GET_CAPS, DCMT_TYPE_ID,
                        make_caps_payload(BREAD_CAPS_SCHEMA_V1,
                                          DCMT_CAP_LEVEL_3,
                                          level3_flags));

    auto session = make_session(transport);
    const DiscoveryResult result = run_discovery(*session, make_scan_config());

    ASSERT_EQ(result.devices.size(), 1u);
    EXPECT_EQ(result.devices[0].address, 0x15);
    EXPECT_EQ(result.devices[0].type, DeviceType::Dcmt);
    EXPECT_EQ(result.devices[0].capability_profile.source,
              inventory::CapabilitySource::Queried);
    EXPECT_EQ(result.devices[0].capability_profile.level, DCMT_CAP_LEVEL_3);
    EXPECT_EQ(result.inventory_mode, "discovered");
}

} // namespace anolis_provider_bread::startup
