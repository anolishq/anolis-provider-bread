#pragma once

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

struct SessionStatus {
    SessionErrorCode code = SessionErrorCode::Ok;
    std::string message;
    int native_code = 0;
    int attempts = 0;

    bool ok() const {
        return code == SessionErrorCode::Ok;
    }

    explicit operator bool() const {
        return ok();
    }

    static SessionStatus success() {
        return {};
    }

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

struct RawFrame {
    uint8_t type_id = 0;
    uint8_t opcode = 0;
    std::vector<uint8_t> payload;
};

struct ScanResult {
    uint8_t address = 0;
    bool has_type_id = false;
    uint8_t type_id = 0;
};

struct SessionOptions {
    std::string bus_path;
    uint32_t query_delay_us = 10000u;
    uint32_t timeout_ms = 100u;
    int retry_count = 2;
};

struct ScanOptions {
    uint8_t start_address = kMinI2cAddress;
    uint8_t end_address = kMaxI2cAddress;
    bool strict = false;
    std::size_t max_results = kDefaultMaxScanResults;
};

class Transport {
public:
    virtual ~Transport() = default;

    virtual SessionStatus open(const SessionOptions &options) = 0;
    virtual void close() noexcept = 0;
    virtual bool is_open() const = 0;
    virtual SessionStatus scan(const ScanOptions &options, std::vector<ScanResult> &out) = 0;
    virtual SessionStatus send(uint8_t address, const RawFrame &frame) = 0;
    virtual SessionStatus read(uint8_t address, RawFrame &frame, uint32_t timeout_us) = 0;
    virtual void delay_us(uint32_t delay_us) = 0;
};

class Session {
public:
    Session(Transport &transport, SessionOptions options);

    const SessionOptions &options() const;
    bool is_open() const;

    SessionStatus open();
    void close() noexcept;

    SessionStatus scan(const ScanOptions &options, std::vector<ScanResult> &out);
    SessionStatus send(uint8_t address, const RawFrame &frame);
    SessionStatus read(uint8_t address, RawFrame &frame);
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

SessionOptions make_session_options(const ProviderConfig &config);
bool is_retryable(SessionErrorCode code);
std::string to_string(SessionErrorCode code);

} // namespace anolis_provider_bread::crumbs