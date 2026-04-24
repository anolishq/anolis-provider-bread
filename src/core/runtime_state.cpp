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

#include "core/startup.hpp"
#include "crumbs/session.hpp"
#include "logging/logger.hpp"

#if defined(__linux__)
#include "crumbs/linux_transport.hpp"
#endif

namespace anolis_provider_bread::runtime {
namespace {

std::mutex g_mutex;
RuntimeState g_state;

// Session ownership: transport must outlive session (declared first, destroyed
// last).
std::unique_ptr<crumbs::Transport> g_transport;
std::unique_ptr<crumbs::Session> g_session;

bool is_mock_mode(const ProviderConfig &config) {
  return config.bus_path.rfind("mock://", 0) == 0;
}

std::string build_startup_message(int device_count, int unsupported_count,
                                  const std::vector<std::string> &missing_ids,
                                  const std::string &inventory_mode) {
  std::ostringstream msg;
  msg << "BREAD discovery complete (" << inventory_mode << "): " << device_count
      << " device(s) active";
  if (unsupported_count > 0) {
    msg << ", " << unsupported_count << " unsupported";
  }
  if (!missing_ids.empty()) {
    msg << ", " << missing_ids.size() << " expected device(s) missing";
  }
  return msg.str();
}

} // namespace

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
    auto transport = std::make_unique<crumbs::LinuxTransport>();
    auto sess = std::make_unique<crumbs::Session>(
        *transport, crumbs::make_session_options(config));

    const crumbs::SessionStatus open_status = sess->open();
    if (!open_status) {
      throw std::runtime_error("failed to open CRUMBS bus '" + config.bus_path +
                               "': " + open_status.message);
    }

    startup::DiscoveryResult discovery = startup::run_discovery(*sess, config);

    state.devices = std::move(discovery.devices);
    state.inventory_mode = discovery.inventory_mode;
    state.unsupported_probe_count =
        static_cast<int>(discovery.unsupported_probes.size());
    state.missing_expected_ids = std::move(discovery.missing_expected_ids);
    state.ready = true;
    state.startup_message = build_startup_message(
        static_cast<int>(state.devices.size()), state.unsupported_probe_count,
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
  state.inventory_mode =
      inventory::to_string(inventory::InventorySource::ConfigSeeded);
  state.unsupported_probe_count = 0;
  state.ready = true;
  state.startup_message = "config-seeded inventory (mock mode)";

  std::lock_guard<std::mutex> lock(g_mutex);
  g_state = std::move(state);
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
