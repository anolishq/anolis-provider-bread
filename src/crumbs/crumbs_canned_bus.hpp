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
 *     the pending SET_REPLY opcode (or the SET_WATCHDOG timeout) per address;
 *   - a read builds the matching GET_STATE / GET_WATCHDOG reply, encodes it, and
 *     returns it padded with 0xFF to the requested count — exactly what a real
 *     fixed-count i2c-dev read of a short CRUMBS frame yields.
 *
 * Seed device types with `add_device` before opening (as `MockTransport` did).
 */

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

#include "anolis/provider_sdk/i2c/i2c_bus.hpp"
#include "anolis/provider_sdk/i2c/i2c_status.hpp"
#include "anolis/provider_sdk/i2c/io_stats.hpp"

namespace anolis_provider_bread::crumbs {

class CrumbsCannedBus final : public anolis::provider_sdk::i2c::I2cBus {
public:
    explicit CrumbsCannedBus(std::string bus_path);

    /** @brief Seed a simulated device's BREAD type id at @p address. */
    void add_device(uint8_t address, uint8_t type_id);

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
