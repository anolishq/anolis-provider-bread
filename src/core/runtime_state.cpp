#include "core/runtime_state.hpp"

#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "core/startup.hpp"
#include "crumbs/session.hpp"
#include "logging/logger.hpp"

#if defined(ANOLIS_PROVIDER_BREAD_HAS_CRUMBS)
#include "crumbs/linux_transport.hpp"
#endif

namespace anolis_provider_bread::runtime {
namespace {

std::mutex g_mutex;
RuntimeState g_state;

// Session ownership: transport must outlive session (declared first, destroyed last).
std::unique_ptr<crumbs::Transport> g_transport;
std::unique_ptr<crumbs::Session>   g_session;

#if defined(ANOLIS_PROVIDER_BREAD_HAS_CRUMBS)
std::string build_startup_message(int device_count,
                                   int unsupported_count,
                                   const std::vector<std::string> &missing_ids,
                                   const std::string &inventory_mode) {
    std::ostringstream msg;
    msg << "BREAD discovery complete (" << inventory_mode << "): "
        << device_count << " device(s) active";
    if (unsupported_count > 0) {
        msg << ", " << unsupported_count << " unsupported";
    }
    if (!missing_ids.empty()) {
        msg << ", " << missing_ids.size() << " expected device(s) missing";
    }
    return msg.str();
}
#endif // ANOLIS_PROVIDER_BREAD_HAS_CRUMBS

} // namespace

void reset() {
    // Destroy session before transport (correct order, outside the lock).
    std::unique_ptr<crumbs::Session>   old_session;
    std::unique_ptr<crumbs::Transport> old_transport;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_state    = RuntimeState{};
        old_session   = std::move(g_session);
        old_transport = std::move(g_transport);
    }
    // old_session destroyed first (stack unwind order), then old_transport. Correct.
}

void initialize(const ProviderConfig &config) {
    RuntimeState state;
    state.config     = config;
    state.started_at = std::chrono::system_clock::now();

#if defined(ANOLIS_PROVIDER_BREAD_HAS_CRUMBS)
    // Real hardware path: open the CRUMBS bus and run discovery.
    auto transport = std::make_unique<crumbs::LinuxTransport>();
    auto sess      = std::make_unique<crumbs::Session>(
        *transport, crumbs::make_session_options(config));

    const crumbs::SessionStatus open_status = sess->open();
    if (!open_status) {
        throw std::runtime_error("failed to open CRUMBS bus '" + config.bus_path +
                                 "': " + open_status.message);
    }

    startup::DiscoveryResult discovery = startup::run_discovery(*sess, config);

    state.devices              = std::move(discovery.devices);
    state.inventory_mode       = discovery.inventory_mode;
    state.unsupported_probe_count =
        static_cast<int>(discovery.unsupported_probes.size());
    state.missing_expected_ids = std::move(discovery.missing_expected_ids);
    state.ready                = true;
    state.startup_message      = build_startup_message(
        static_cast<int>(state.devices.size()),
        state.unsupported_probe_count,
        state.missing_expected_ids,
        state.inventory_mode);

    logging::info(state.startup_message);

    std::unique_ptr<crumbs::Session>   old_session;
    std::unique_ptr<crumbs::Transport> old_transport;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_state       = std::move(state);
        old_session   = std::move(g_session);
        old_transport = std::move(g_transport);
        g_session     = std::move(sess);
        g_transport   = std::move(transport);
    }
    // Old session/transport destroyed here, outside the lock.

#else
    // No-hardware path: seed inventory from config.
    if(config.require_live_session) {
        throw std::runtime_error(
            "hardware.require_live_session=true but provider was built without hardware support "
            "(ANOLIS_PROVIDER_BREAD_ENABLE_HARDWARE=OFF). Rebuild with dev-linux-hardware-release.");
    }

    state.devices         = inventory::build_seed_inventory(config);
    state.inventory_mode  = inventory::to_string(inventory::InventorySource::ConfigSeeded);
    state.unsupported_probe_count = 0;
    state.ready           = true;
    state.startup_message = "config-seeded inventory (hardware integration disabled)";

    std::lock_guard<std::mutex> lock(g_mutex);
    g_state = std::move(state);
    // g_transport and g_session remain null on non-hardware builds.
#endif
}

RuntimeState snapshot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state;
}

crumbs::Session *session() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_session.get();
}

} // namespace anolis_provider_bread::runtime
