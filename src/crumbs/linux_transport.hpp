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
 * @brief Length of the CRUMBS frame contained in a raw I2C read, per its header.
 *
 * A Linux I2C controller must request a fixed byte count up front, but a CRUMBS
 * peripheral writes only its actual frame (`Wire.write(frame, frame_len)`) and
 * the bus then floats high — so a raw read yields the *buffer* size, with 0xFF
 * padding in the tail, not the *frame* size.
 *
 * CRUMBS documents an exact-frame-length contract for `crumbs_decode_message()`.
 * v0.12.2 did not enforce it (trailing bytes were ignored); v0.12.4 does, and
 * returns -1 for them. Reads must therefore be trimmed to this length before
 * decoding — passing the raw length rejects every otherwise-valid reply.
 *
 * @param buffer      Bytes returned by the raw read.
 * @param bytes_read  Number of bytes the read returned.
 * @param frame_len   Set to the declared frame length on success.
 */
SessionStatus crumbs_reply_frame_length(const uint8_t *buffer, std::size_t bytes_read, std::size_t &frame_len);

/**
 * @brief Concrete transport that binds the generic session API to
 * `crumbs_linux`.
 *
 * Responsibilities:
 * Owns the Linux I2C controller state, converts transport failures into
 * `SessionStatus`, and provides the C ABI handle used by BREAD helper calls.
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

    /**
     * @brief Build a C ABI device handle bound to this transport's live context.
     *
     * The returned handle borrows transport-owned state and becomes invalid
     * after `close()`.
     */
    crumbs_device_t bind_device(uint8_t address);

private:
    crumbs_context_t ctx_{};
    crumbs_linux_i2c_t i2c_{};
    uint32_t timeout_us_ = 0u;
    bool open_ = false;
};

}  // namespace anolis_provider_bread::crumbs

#endif  // defined(__linux__)
