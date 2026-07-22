/**
 * @file runtime_state.cpp
 * @brief Process-wide runtime state ownership for the BREAD provider.
 *
 * This module owns the startup snapshot plus the optional live CRUMBS session.
 * Successful reinitialization swaps the new session and snapshot in together so
 * handlers never observe a mismatched inventory/session pair.
 */

#include "core/runtime_state.hpp"

#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "anolis/provider_sdk/i2c/fault_injecting_i2c_bus.hpp"
#include "anolis/provider_sdk/i2c/i2c_bus.hpp"
#include "core/startup.hpp"
#include "crumbs/crumbs_canned_bus.hpp"
#include "crumbs/crumbs_transport.hpp"
#include "crumbs/session.hpp"
#include "devices/common/bread_compatibility.hpp"
#include "devices/common/watchdog.hpp"
#include "logging/logger.hpp"

#if defined(__linux__)
#include "anolis/provider_sdk/i2c/linux_i2c_bus.hpp"
#endif

namespace anolis_provider_bread::runtime {
namespace {

std::mutex g_mutex;
RuntimeState g_state;

// Session ownership: transport must outlive session (declared first, destroyed
// last).
std::unique_ptr<crumbs::Transport> g_transport;
std::unique_ptr<crumbs::Session> g_session;

bool is_mock_mode(const ProviderConfig &config) { return config.bus_path.rfind("mock://", 0) == 0; }

std::string build_startup_message(int device_count, int unsupported_count, const std::vector<std::string> &missing_ids,
                                  const std::string &inventory_mode) {
    std::ostringstream msg;
    msg << "BREAD discovery complete (" << inventory_mode << "): " << device_count << " device(s) active";
    if (unsupported_count > 0) {
        msg << ", " << unsupported_count << " unsupported";
    }
    if (!missing_ids.empty()) {
        msg << ", " << missing_ids.size() << " expected device(s) missing";
    }
    return msg.str();
}

}  // namespace

void reset() {
    // Destroy session before transport, and do it outside the lock so teardown
    // does not block unrelated snapshot readers longer than necessary.
    std::unique_ptr<crumbs::Session> old_session;
    std::unique_ptr<crumbs::Transport> old_transport;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_state = RuntimeState{};
        old_session = std::move(g_session);
        old_transport = std::move(g_transport);
    }
    // old_session destroyed first (stack unwind order), then old_transport.
    // Correct.
}

void initialize(const ProviderConfig &config) {
    RuntimeState state;
    state.config = config;
    state.started_at = std::chrono::system_clock::now();

#if defined(__linux__)
    if (!is_mock_mode(config)) {
        // Hardware path: open one live CRUMBS session and derive the runtime
        // inventory from an immediate discovery pass on that bus.
        auto bus = std::make_unique<anolis::provider_sdk::i2c::LinuxI2cBus>(config.bus_path, config.timeout_ms,
                                                                            config.retry_count);
        auto transport = std::make_unique<crumbs::CrumbsTransport>(std::move(bus));
        auto sess = std::make_unique<crumbs::Session>(*transport, crumbs::make_session_options(config));

        const crumbs::SessionStatus open_status = sess->open();
        if (!open_status) {
            throw std::runtime_error("failed to open CRUMBS bus '" + config.bus_path + "': " + open_status.message);
        }

        startup::DiscoveryResult discovery = startup::run_discovery(*sess, config);

        // Arm configured firmware command watchdogs once the roster is known
        // (#112). Capability gating and failure logging live in the helper.
        for (const auto &device : discovery.devices) {
            watchdog::arm_if_configured(*sess, device, "startup");
        }

        state.devices = std::move(discovery.devices);
        state.inventory_mode = discovery.inventory_mode;
        state.unsupported_probe_count = static_cast<int>(discovery.unsupported_probes.size());
        state.missing_expected_ids = std::move(discovery.missing_expected_ids);
        state.missing_expected_details = std::move(discovery.missing_expected_details);
        state.ready = true;
        state.ready_at = std::chrono::system_clock::now();
        state.startup_message =
            build_startup_message(static_cast<int>(state.devices.size()), state.unsupported_probe_count,
                                  state.missing_expected_ids, state.inventory_mode);

        logging::info(state.startup_message);

        std::unique_ptr<crumbs::Session> old_session;
        std::unique_ptr<crumbs::Transport> old_transport;
        {
            // Publish the new runtime snapshot and its session together so callers
            // do not see a new inventory with an old session, or vice versa.
            std::lock_guard<std::mutex> lock(g_mutex);
            g_state = std::move(state);
            old_session = std::move(g_session);
            old_transport = std::move(g_transport);
            g_session = std::move(sess);
            g_transport = std::move(transport);
        }
        // Old session/transport destroyed here, outside the lock.
        return;
    }
#endif

    // Mock / config-seeded path: build inventory from config without bus access.
    state.devices = inventory::build_seed_inventory(config);
    state.inventory_mode = inventory::to_string(inventory::InventorySource::ConfigSeeded);
    state.unsupported_probe_count = 0;
    state.ready = true;
    state.ready_at = std::chrono::system_clock::now();
    state.startup_message = "config-seeded inventory (mock mode)";

    // Stand up a mock CRUMBS session over a canned bus so reads/calls exercise
    // the real transport + CRUMBS decode path (not a synthesized RawFrame),
    // returning real declared data. A fault spec on the mock:// query wraps the
    // canned bus in the fault-injecting decorator (anolishq/anolis#99); plain
    // mock:// is a clean, fault-free device. Inventory stays config-seeded above.
    auto [bus_path, fault_query] = anolis::provider_sdk::i2c::split_bus_query(config.bus_path);
    auto canned = std::make_unique<crumbs::CrumbsCannedBus>(bus_path);
    for (const auto &device : state.devices) {
        canned->add_device(static_cast<uint8_t>(device.address), inventory::bread_type_id(device.type));
    }
    std::unique_ptr<anolis::provider_sdk::i2c::I2cBus> bus;
    const auto fault_spec = anolis::provider_sdk::i2c::FaultSpec::parse(fault_query);
    if (fault_spec.any()) {
        bus = std::make_unique<anolis::provider_sdk::i2c::FaultInjectingI2cBus>(std::move(canned), fault_spec);
    } else {
        bus = std::move(canned);
    }
    auto transport = std::make_unique<crumbs::CrumbsTransport>(std::move(bus));
    auto sess = std::make_unique<crumbs::Session>(*transport, crumbs::make_session_options(config));
    const crumbs::SessionStatus open_status = sess->open();
    if (!open_status) {
        throw std::runtime_error("failed to open mock CRUMBS bus '" + config.bus_path + "': " + open_status.message);
    }

    // Mirror the hardware path so mock rehearsals exercise watchdog arming
    // (the canned bus simulates the arming state for GET_WATCHDOG queries).
    for (const auto &device : state.devices) {
        watchdog::arm_if_configured(*sess, device, "startup");
    }

    std::unique_ptr<crumbs::Session> old_session;
    std::unique_ptr<crumbs::Transport> old_transport;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_state = std::move(state);
        old_session = std::move(g_session);
        old_transport = std::move(g_transport);
        g_session = std::move(sess);
        g_transport = std::move(transport);
    }
    // Old session/transport destroyed here, outside the lock.
}

RuntimeState snapshot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state;
}

crumbs::Session *session() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_session.get();
}

}  // namespace anolis_provider_bread::runtime
