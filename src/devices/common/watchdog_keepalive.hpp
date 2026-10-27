#pragma once

/**
 * @file watchdog_keepalive.hpp
 * @brief Feeds armed command watchdogs until the runtime's first call (#190).
 *
 * Bread arms each board's watchdog at the end of discovery, and nothing reaches
 * the boards again until the runtime's first ReadSignals or Call, which waits
 * for every other provider to start. Every valid frame refreshes the firmware
 * watchdog, so from arming until that first contact the keepalive sends each
 * armed device a GET_WATCHDOG read every quarter of its timeout. A board stays
 * armed across a provider restart, so arming later would not close the gap.
 *
 * Feeding stops per device at its first contact, and for every device at
 * kKeepaliveCap: a runtime that never starts calling still lets the boards trip.
 */

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "crumbs/session.hpp"
#include "devices/common/inventory.hpp"

namespace anolis_provider_bread::watchdog {

/** @brief How long after arming the keepalive gives up on the runtime. */
inline constexpr std::chrono::milliseconds kKeepaliveCap{30000};

class Keepalive {
public:
    using Clock = std::chrono::steady_clock;

    /**
     * @brief Feed the devices in `devices` with a configured and supported
     * watchdog, counting from `armed_at`. Nothing is sent until tick() or start().
     */
    Keepalive(crumbs::Session &session, const std::vector<inventory::InventoryDevice> &devices,
              std::chrono::milliseconds cap = kKeepaliveCap, Clock::time_point armed_at = Clock::now());
    ~Keepalive();

    Keepalive(const Keepalive &) = delete;
    Keepalive &operator=(const Keepalive &) = delete;

    /** @brief Run tick() on a thread at each due time until it returns nullopt. */
    void start();

    /** @brief The runtime has called `device_id`: stop feeding it, and log how long that took. */
    void contacted(const std::string &device_id, Clock::time_point now = Clock::now());

    /**
     * @brief Feed every device due at `now`. Returns when the next one is due,
     * or nullopt once every device has been contacted or the cap has passed.
     */
    std::optional<Clock::time_point> tick(Clock::time_point now);

private:
    struct Fed {
        inventory::InventoryDevice device;
        Clock::duration interval;
        Clock::time_point next_due;
        bool done = false;
    };

    std::optional<Clock::time_point> tick_locked(Clock::time_point now);
    void run();

    crumbs::Session &session_;
    Clock::time_point armed_at_;
    Clock::time_point deadline_;
    std::vector<Fed> fed_;
    bool capped_ = false;
    bool stopping_ = false;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
};

}  // namespace anolis_provider_bread::watchdog
