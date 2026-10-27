#include "devices/common/watchdog_keepalive.hpp"

#include <algorithm>

#include "devices/common/watchdog.hpp"
#include "logging/logger.hpp"

namespace anolis_provider_bread::watchdog {
namespace {

std::string tag(const inventory::InventoryDevice &device) {
    return device.descriptor.device_id() + " (" + format_i2c_address(device.address) + ")";
}

long long ms_between(Keepalive::Clock::time_point from, Keepalive::Clock::time_point to) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
}

}  // namespace

Keepalive::Keepalive(crumbs::Session &session, const std::vector<inventory::InventoryDevice> &devices,
                     std::chrono::milliseconds cap, Clock::time_point armed_at)
    : session_(session), armed_at_(armed_at), deadline_(armed_at + cap) {
    for (const auto &device : devices) {
        if (device.command_watchdog_ms <= 0 || !capability_supported(device)) {
            continue;
        }
        const Clock::duration interval = std::max<Clock::duration>(
            std::chrono::milliseconds{device.command_watchdog_ms} / 4, std::chrono::milliseconds{1});
        fed_.push_back(Fed{device, interval, armed_at + interval});
    }
}

Keepalive::~Keepalive() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void Keepalive::start() {
    if (!fed_.empty()) {
        thread_ = std::thread([this] { run(); });
    }
}

void Keepalive::contacted(const std::string &device_id, Clock::time_point now) {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (auto &fed : fed_) {
        if (fed.done || fed.device.descriptor.device_id() != device_id) {
            continue;
        }
        fed.done = true;
        if (!capped_) {
            logging::info("watchdog: " + tag(fed.device) + " first call " + std::to_string(ms_between(armed_at_, now)) +
                          " ms after arming; fed every " +
                          std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(fed.interval).count()) +
                          " ms until then");
        }
    }
}

std::optional<Keepalive::Clock::time_point> Keepalive::tick(Clock::time_point now) {
    const std::lock_guard<std::mutex> lock(mutex_);
    return tick_locked(now);
}

std::optional<Keepalive::Clock::time_point> Keepalive::tick_locked(Clock::time_point now) {
    if (capped_) {
        return std::nullopt;
    }
    if (now >= deadline_) {
        capped_ = true;
        for (const auto &fed : fed_) {
            if (!fed.done) {
                logging::warning("watchdog: no call from the runtime for " + tag(fed.device) + " within " +
                                 std::to_string(ms_between(armed_at_, deadline_)) +
                                 " ms of arming; no longer feeding its watchdog");
            }
        }
        return std::nullopt;
    }
    std::optional<Clock::time_point> next;
    for (auto &fed : fed_) {
        if (fed.done) {
            continue;
        }
        if (fed.next_due <= now) {
            WatchdogStatus ignored;
            (void)query_status(session_, fed.device, ignored);
            fed.next_due = now + fed.interval;
        }
        next = next ? std::min(*next, fed.next_due) : fed.next_due;
    }
    if (next) {
        next = std::min(*next, deadline_);
    }
    return next;
}

void Keepalive::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        const std::optional<Clock::time_point> next = tick_locked(Clock::now());
        if (!next) {
            return;
        }
        wake_.wait_until(lock, *next, [this] { return stopping_; });
    }
}

}  // namespace anolis_provider_bread::watchdog
