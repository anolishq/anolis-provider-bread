#pragma once

/**
 * @file linux_transport.hpp
 * @brief Linux-backed CRUMBS transport implementation used by live BREAD
 * sessions.
 */

#include "crumbs/session.hpp"

#if defined(__linux__)

extern "C" {
#include "crumbs.h"
#include "crumbs_linux.h"
}

namespace anolis_provider_bread::crumbs {

/**
 * @brief Concrete transport that binds the generic session API to
 * `crumbs_linux`.
 *
 * Responsibilities:
 * Owns the Linux I2C controller state and converts transport failures into
 * `SessionStatus`. Raw reads are trimmed to the header-declared frame length
 * via CRUMBS' `crumbs_frame_length()` before decoding (a fixed-count i2c-dev
 * read returns bus padding after the frame).
 *
 * Threading:
 * This transport is not internally synchronized. Callers are expected to
 * serialize session access at a higher layer.
 */
class LinuxTransport final : public Transport {
public:
    /** @brief Open the configured I2C bus and initialize the CRUMBS controller
     * context. */
    SessionStatus open(const SessionOptions &options) override;

    /** @brief Close the Linux transport and release any live controller
     * resources. */
    void close() noexcept override;

    /** @brief Report whether the Linux transport currently owns an open
     * controller. */
    bool is_open() const override;

    /** @brief Scan the configured bus for CRUMBS devices and their type
     * identifiers. */
    SessionStatus scan(const ScanOptions &options, std::vector<ScanResult> &out) override;

    /** @brief Send one encoded CRUMBS frame to the target address. */
    SessionStatus send(uint8_t address, const RawFrame &frame) override;

    /** @brief Read and decode one CRUMBS reply frame from the target address. */
    SessionStatus read(uint8_t address, RawFrame &frame, uint32_t timeout_us) override;

    /** @brief Sleep through the platform-specific delay hook expected by some
     * BREAD helpers. */
    void delay_us(uint32_t delay_us) override;

private:
    crumbs_context_t ctx_{};
    crumbs_linux_i2c_t i2c_{};
    uint32_t timeout_us_ = 0u;
    bool open_ = false;
};

}  // namespace anolis_provider_bread::crumbs

#endif  // defined(__linux__)
