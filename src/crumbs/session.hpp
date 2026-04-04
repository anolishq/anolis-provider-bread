#pragma once

/**
 * @file session.hpp
 * @brief Serialized CRUMBS session and transport contracts for the BREAD provider.
 */

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "config/provider_config.hpp"

namespace anolis_provider_bread::crumbs {

constexpr uint8_t kSetReplyOpcode = 0xFE;
constexpr std::size_t kMaxPayloadBytes = 27u;
constexpr uint8_t kMinI2cAddress = 0x08u;
constexpr uint8_t kMaxI2cAddress = 0x77u;
constexpr std::size_t kDefaultMaxScanResults = 32u;

/**
 * @brief Provider-local error codes for CRUMBS session operations.
 */
enum class SessionErrorCode {
    Ok,
    InvalidArgument,
    NotOpen,
    AlreadyOpen,
    OpenFailed,
    ScanFailed,
    WriteFailed,
    ReadFailed,
    Timeout,
    DecodeFailed,
    TransportError,
};

/**
 * @brief Lightweight status object returned by CRUMBS transport/session APIs.
 */
struct SessionStatus {
    SessionErrorCode code = SessionErrorCode::Ok;
    std::string message;
    int native_code = 0;
    int attempts = 0;

    /** @brief Report whether the status represents success. */
    bool ok() const {
        return code == SessionErrorCode::Ok;
    }

    /** @brief Allow status objects to be used directly in boolean checks. */
    explicit operator bool() const {
        return ok();
    }

    /** @brief Construct the canonical success status. */
    static SessionStatus success() {
        return {};
    }

    /** @brief Construct a failure status with optional native transport details. */
    static SessionStatus failure(SessionErrorCode code_in,
                                 std::string message_in,
                                 int native_code_in = 0,
                                 int attempts_in = 0) {
        SessionStatus status;
        status.code = code_in;
        status.message = std::move(message_in);
        status.native_code = native_code_in;
        status.attempts = attempts_in;
        return status;
    }
};

/**
 * @brief Raw CRUMBS frame used by the transport layer.
 */
struct RawFrame {
    uint8_t type_id = 0;
    uint8_t opcode = 0;
    std::vector<uint8_t> payload;
};

/**
 * @brief One discovered address returned from a CRUMBS bus scan.
 */
struct ScanResult {
    uint8_t address = 0;
    bool has_type_id = false;
    uint8_t type_id = 0;
};

/**
 * @brief Session-level transport options derived from provider config.
 */
struct SessionOptions {
    std::string bus_path;
    uint32_t query_delay_us = 10000u;
    uint32_t timeout_ms = 100u;
    int retry_count = 2;
};

/**
 * @brief Options controlling a CRUMBS scan operation.
 */
struct ScanOptions {
    uint8_t start_address = kMinI2cAddress;
    uint8_t end_address = kMaxI2cAddress;
    bool strict = false;
    std::size_t max_results = kDefaultMaxScanResults;
};

/**
 * @brief Abstract transport backend used by `Session`.
 *
 * Implementations own the platform-specific bus mechanics; `Session` layers
 * validation, retry, delay, and serialized access on top.
 */
class Transport {
public:
    virtual ~Transport() = default;

    /** @brief Open the transport using the provided session options. */
    virtual SessionStatus open(const SessionOptions &options) = 0;

    /** @brief Close the transport and release underlying resources. */
    virtual void close() noexcept = 0;

    /** @brief Report whether the transport is currently open. */
    virtual bool is_open() const = 0;

    /** @brief Scan the configured bus range for CRUMBS responders. */
    virtual SessionStatus scan(const ScanOptions &options, std::vector<ScanResult> &out) = 0;

    /** @brief Send one raw frame to a device address. */
    virtual SessionStatus send(uint8_t address, const RawFrame &frame) = 0;

    /** @brief Read one raw frame from a device address with a timeout. */
    virtual SessionStatus read(uint8_t address, RawFrame &frame, uint32_t timeout_us) = 0;

    /** @brief Sleep for a transport-appropriate delay between request phases. */
    virtual void delay_us(uint32_t delay_us) = 0;
};

/**
 * @brief Serialized CRUMBS session facade used by startup and device adapters.
 *
 * Threading:
 * All public operations take an internal mutex so scan/send/read/query
 * sequences do not interleave on the shared transport.
 *
 * Error handling:
 * Retry behavior is driven by `SessionOptions::retry_count`. Per-operation
 * failures are returned as `SessionStatus` rather than thrown.
 */
class Session {
public:
    Session(Transport &transport, SessionOptions options);

    /** @brief Return the immutable options used by this session. */
    const SessionOptions &options() const;

    /** @brief Report whether the underlying transport is open. */
    bool is_open() const;

    /** @brief Open the underlying transport after validating session options. */
    SessionStatus open();

    /** @brief Close the underlying transport. */
    void close() noexcept;

    /** @brief Scan the configured bus range for CRUMBS responders. */
    SessionStatus scan(const ScanOptions &options, std::vector<ScanResult> &out);

    /** @brief Send one CRUMBS frame to a device address. */
    SessionStatus send(uint8_t address, const RawFrame &frame);

    /** @brief Read one CRUMBS frame from a device address. */
    SessionStatus read(uint8_t address, RawFrame &frame);

    /**
     * @brief Execute the CRUMBS SET_REPLY query-read sequence.
     *
     * This sends the `kSetReplyOpcode` request, waits `query_delay_us`, and
     * then reads the reply frame under one session lock.
     */
    SessionStatus query_read(uint8_t address, uint8_t reply_opcode, RawFrame &out);

private:
    SessionStatus validate_open_options() const;
    SessionStatus validate_scan_options(const ScanOptions &options) const;
    SessionStatus validate_address(uint8_t address) const;
    SessionStatus validate_frame(const RawFrame &frame) const;

    Transport &transport_;
    SessionOptions options_;
    mutable std::mutex mutex_;
};

/** @brief Build session transport options from resolved provider config. */
SessionOptions make_session_options(const ProviderConfig &config);

/** @brief Report whether a session error is eligible for retry. */
bool is_retryable(SessionErrorCode code);

/** @brief Convert a session error code to a stable debug string. */
std::string to_string(SessionErrorCode code);

} // namespace anolis_provider_bread::crumbs
