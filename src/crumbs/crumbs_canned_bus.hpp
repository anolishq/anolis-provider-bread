#pragma once

/**
 * @file crumbs_canned_bus.hpp
 * @brief A canned CRUMBS device on the shared I2C bus, for mock sessions.
 *
 * The Level-2 mock (anolishq/anolis-provider-sdk#19): the former `MockTransport`
 * synthesized finished `RawFrame`s *above* the CRUMBS codec, so mock mode never
 * ran `crumbs_frame_length` / `crumbs_decode_message` — the exact-length trim +
 * decode that bread#97 is about. This bus sits *below* the codec: it answers the
 * CRUMBS wire protocol in real, CRC'd, **padded** bytes, so `CrumbsTransport`'s
 * real read path (`bus.read` -> `crumbs_frame_length` -> `crumbs_decode_message`)
 * runs on them. Wrapped in `FaultInjectingI2cBus`, it also gives always-on
 * fault-injection coverage over the decode path (anolishq/anolis#99).
 *
 * It is the byte-level twin of `MockTransport`'s request/reply state machine:
 *   - a write is a fully-encoded CRUMBS request frame; it is decoded to record
 *     the pending SET_REPLY opcode (or the SET_WATCHDOG timeout, or a
 *     CLEAR_WATCHDOG_TRIP) per address;
 *   - a read builds the matching GET_STATE / GET_WATCHDOG reply, encodes it, and
 *     returns it padded with 0xFF to the requested count — exactly what a real
 *     fixed-count i2c-dev read of a short CRUMBS frame yields;
 *   - every command frame other than SET_REPLY is counted for GET_ACK, as a
 *     CRUMBS 0.16 peripheral counts it: applied, or refused, rejected or lost
 *     where a test injects that.
 *
 * Seed device types with `add_device` before opening (as `MockTransport` did).
 */

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <utility>

#include "anolis/provider_sdk/i2c/i2c_bus.hpp"
#include "anolis/provider_sdk/i2c/i2c_status.hpp"
#include "anolis/provider_sdk/i2c/io_stats.hpp"

namespace anolis_provider_bread::crumbs {

class CrumbsCannedBus final : public anolis::provider_sdk::i2c::I2cBus {
public:
    explicit CrumbsCannedBus(std::string bus_path);

    /** @brief Seed a simulated device's BREAD type id at @p address. */
    void add_device(uint8_t address, uint8_t type_id);

    /**
     * @brief Simulate a command-watchdog expiry at @p address.
     *
     * The canned device models latching firmware: the trip survives
     * SET_WATCHDOG and is released only by CLEAR_WATCHDOG_TRIP.
     */
    void trip_watchdog(uint8_t address);

    /** @brief The next command frame at @p address is refused with @p reason
     * (a BREAD_REFUSE_* byte) and not acted on. */
    void refuse_next(uint8_t address, uint8_t reason);

    /** @brief The next command frame at @p address is rejected with @p reason
     * (a CRUMBS_ACK_REJECT_* byte) and not acted on. */
    void reject_next(uint8_t address, uint8_t reason);

    /** @brief The next @p count command frames at @p address never arrive:
     * not counted, not acted on. */
    void drop_next(uint8_t address, int count);

    /** @brief After @p skip more GET_ACK reads succeed, the next @p count fail
     * at @p address. */
    void fail_ack_reads(uint8_t address, int count, int skip = 0);

    /** @brief Command frames with @p opcode written to @p address, lost ones
     * included. */
    int writes_of(uint8_t address, uint8_t opcode) const;

    anolis::provider_sdk::i2c::I2cStatus open() override;
    void close() override;
    bool is_open() const override;
    const std::string &bus_path() const override;

    anolis::provider_sdk::i2c::I2cStatus write(uint8_t address, const uint8_t *tx_data, size_t tx_len) override;
    anolis::provider_sdk::i2c::I2cStatus read(uint8_t address, uint8_t *rx_data, size_t rx_len, size_t *rx_received,
                                              uint32_t timeout_us) override;
    anolis::provider_sdk::i2c::I2cStatus write_then_read(uint8_t address, const uint8_t *tx_data, size_t tx_len,
                                                         uint8_t *rx_data, size_t rx_len, size_t *rx_received) override;
    void delay_us(uint32_t delay_us) override;
    anolis::provider_sdk::i2c::IoStats io_stats_for(uint8_t address) const override;

private:
    struct DeviceState {
        uint8_t type_id = 0;
        uint8_t pending_reply_opcode = 0;
        bool has_pending = false;
        uint16_t watchdog_timeout_ms = 0;
        bool watchdog_tripped = false;
        uint8_t watchdog_trip_count = 0;
        // GET_ACK record: [counter][opcode][status][reason].
        uint8_t ack_counter = 0;
        uint8_t ack_opcode = 0;
        uint8_t ack_status = 0;
        uint8_t ack_reason = 0;
        // Outcomes injected for the next command frames, oldest first.
        std::deque<std::pair<uint8_t, uint8_t>> next_outcomes;  // {CRUMBS_ACK_* status, reason}
        int drop_count = 0;
        int ack_read_skip = 0;
        int ack_read_failures = 0;
        std::map<uint8_t, int> writes_by_opcode;
    };

    // Decode a request frame and record the pending reply / watchdog state.
    void apply_write(uint8_t address, const uint8_t *tx_data, size_t tx_len);
    // Build the canned reply for the pending query, encode it, and pad it into
    // rx to the requested count.
    anolis::provider_sdk::i2c::I2cStatus emit_reply(uint8_t address, uint8_t *rx_data, size_t rx_len,
                                                    size_t *rx_received);

    std::string bus_path_;
    bool opened_ = false;
    std::map<uint8_t, DeviceState> devices_;
};

}  // namespace anolis_provider_bread::crumbs
