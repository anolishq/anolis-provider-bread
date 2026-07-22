#pragma once

/**
 * @file crumbs_transport.hpp
 * @brief CRUMBS transport over the shared SDK I2C bus seam.
 *
 * Splits the two concerns the former `LinuxTransport` fused: raw I2C moves to an
 * injected `anolis::provider_sdk::i2c::I2cBus` (real `LinuxI2cBus`, or — in a
 * follow-up — a canned/fault-injecting bus for mock), while CRUMBS framing
 * (encode / frame-length trim / exact-length decode) and scan stay here, driven
 * through the CRUMBS controller helpers over `I2cBus`-backed callbacks.
 *
 * Because the bus is injected, the identical transport serves real hardware and
 * (in the mock-through-bus follow-up) a canned bus that feeds real CRUMBS bytes
 * through the real decode path — the fix for the bread#97 mock blind spot
 * (anolishq/anolis-provider-sdk#19).
 */

#include <cstdint>
#include <memory>
#include <vector>

#include "anolis/provider_sdk/i2c/i2c_bus.hpp"
#include "crumbs/session.hpp"

extern "C" {
#include "crumbs.h"
}

namespace anolis_provider_bread::crumbs {

/**
 * @brief CRUMBS `Transport` backed by a shared `I2cBus`.
 *
 * Threading: not internally synchronized; `Session` serializes access.
 */
class CrumbsTransport final : public Transport {
public:
    explicit CrumbsTransport(std::unique_ptr<anolis::provider_sdk::i2c::I2cBus> bus);

    SessionStatus open(const SessionOptions &options) override;
    void close() noexcept override;
    bool is_open() const override;
    SessionStatus scan(const ScanOptions &options, std::vector<ScanResult> &out) override;
    SessionStatus send(uint8_t address, const RawFrame &frame) override;
    SessionStatus read(uint8_t address, RawFrame &frame, uint32_t timeout_us) override;
    void delay_us(uint32_t delay_us) override;

private:
    std::unique_ptr<anolis::provider_sdk::i2c::I2cBus> bus_;
    crumbs_context_t ctx_{};
    uint32_t scan_timeout_us_ = 0u;
    bool open_ = false;
};

}  // namespace anolis_provider_bread::crumbs
