#pragma once

/**
 * @file mock_transport.hpp
 * @brief In-memory CRUMBS transport used by `mock://` sessions.
 *
 * `MockTransport` is a transport-level device simulator: it satisfies the
 * `crumbs::Transport` contract entirely in memory so a `mock://` session can
 * drive the real `Session`/adapter stack without any I2C hardware. Reads return
 * a valid, canned GET_STATE frame for the seeded device type so the device
 * adapter parsers succeed and yield plausible signal values; sends (control
 * calls) are accepted. This keeps mock mode config-seeded for inventory while
 * still exercising the live read/call code paths.
 */

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "crumbs/session.hpp"

namespace anolis_provider_bread::crumbs {

/**
 * @brief Transport backend that simulates CRUMBS devices in memory.
 *
 * Seed each device address with its BREAD type id via `add_device` before
 * opening; `read` then synthesizes the matching device's GET_STATE frame for the
 * last query issued to that address.
 */
class MockTransport final : public Transport {
public:
    /**
     * @brief Register a simulated device so reads to `address` produce a valid
     * canned state frame for the given BREAD `type_id`.
     */
    void add_device(uint8_t address, uint8_t type_id);

    SessionStatus open(const SessionOptions &options) override;
    void close() noexcept override;
    bool is_open() const override;
    SessionStatus scan(const ScanOptions &options, std::vector<ScanResult> &out) override;
    SessionStatus send(uint8_t address, const RawFrame &frame) override;
    SessionStatus read(uint8_t address, RawFrame &frame, uint32_t timeout_us) override;
    void delay_us(uint32_t delay_us) override;

private:
    bool open_ = false;
    // address -> BREAD type id of the seeded device at that address.
    std::unordered_map<uint8_t, uint8_t> device_type_ids_;
    // address -> reply opcode requested by the most recent SET_REPLY query.
    std::unordered_map<uint8_t, uint8_t> pending_reply_opcode_;
    // address -> simulated command-watchdog timeout (0/absent = disarmed).
    std::unordered_map<uint8_t, uint16_t> watchdog_timeout_ms_;
};

}  // namespace anolis_provider_bread::crumbs
