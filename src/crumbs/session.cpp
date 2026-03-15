#include "crumbs/session.hpp"

#include <algorithm>
#include <optional>
#include <sstream>
#include <utility>

#include "config/provider_config.hpp"
#include "logging/logger.hpp"

namespace anolis_provider_bread::crumbs {
namespace {

std::string describe_address(const std::optional<uint8_t> &address) {
    if(!address.has_value()) {
        return {};
    }
    return " addr=" + format_i2c_address(static_cast<int>(*address));
}

std::string describe_failure(const std::string &operation,
                             const SessionStatus &status,
                             const std::optional<uint8_t> &address) {
    std::ostringstream out;
    out << operation << " failed" << describe_address(address)
        << " code=" << to_string(status.code)
        << " attempts=" << status.attempts;
    if(status.native_code != 0) {
        out << " native_code=" << status.native_code;
    }
    if(!status.message.empty()) {
        out << " message=\"" << status.message << '\"';
    }
    return out.str();
}

template <typename Fn>
SessionStatus execute_with_retry(const SessionOptions &options,
                                 const std::string &operation,
                                 const std::optional<uint8_t> &address,
                                 Fn &&fn) {
    const int max_attempts = std::max(1, options.retry_count + 1);
    SessionStatus last = SessionStatus::failure(
        SessionErrorCode::TransportError,
        operation + " did not run");

    for(int attempt = 1; attempt <= max_attempts; ++attempt) {
        last = fn();
        last.attempts = attempt;
        if(last) {
            return last;
        }

        if(!is_retryable(last.code) || attempt == max_attempts) {
            logging::error(describe_failure(operation, last, address));
            return last;
        }

        logging::warning(
            describe_failure(operation, last, address) + " retrying");
    }

    return last;
}

} // namespace

Session::Session(Transport &transport, SessionOptions options)
    : transport_(transport),
      options_(std::move(options)) {}

const SessionOptions &Session::options() const {
    return options_;
}

bool Session::is_open() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return transport_.is_open();
}

SessionStatus Session::open() {
    std::lock_guard<std::mutex> lock(mutex_);

    if(transport_.is_open()) {
        return SessionStatus::failure(
            SessionErrorCode::AlreadyOpen,
            "CRUMBS session is already open");
    }

    const SessionStatus validation = validate_open_options();
    if(!validation) {
        return validation;
    }

    SessionStatus status = transport_.open(options_);
    status.attempts = 1;
    if(status) {
        logging::info("opened CRUMBS session on bus " + options_.bus_path);
        return status;
    }

    logging::error(describe_failure("open", status, std::nullopt));
    return status;
}

void Session::close() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    transport_.close();
}

SessionStatus Session::scan(const ScanOptions &options, std::vector<ScanResult> &out) {
    std::lock_guard<std::mutex> lock(mutex_);

    const SessionStatus validation = validate_scan_options(options);
    if(!validation) {
        return validation;
    }
    if(!transport_.is_open()) {
        return SessionStatus::failure(
            SessionErrorCode::NotOpen,
            "CRUMBS session is not open");
    }

    out.clear();
    return execute_with_retry(options_, "scan", std::nullopt, [&]() {
        return transport_.scan(options, out);
    });
}

SessionStatus Session::send(uint8_t address, const RawFrame &frame) {
    std::lock_guard<std::mutex> lock(mutex_);

    const SessionStatus address_status = validate_address(address);
    if(!address_status) {
        return address_status;
    }
    const SessionStatus frame_status = validate_frame(frame);
    if(!frame_status) {
        return frame_status;
    }
    if(!transport_.is_open()) {
        return SessionStatus::failure(
            SessionErrorCode::NotOpen,
            "CRUMBS session is not open");
    }

    return execute_with_retry(options_, "send", address, [&]() {
        return transport_.send(address, frame);
    });
}

SessionStatus Session::read(uint8_t address, RawFrame &frame) {
    std::lock_guard<std::mutex> lock(mutex_);

    const SessionStatus address_status = validate_address(address);
    if(!address_status) {
        return address_status;
    }
    if(!transport_.is_open()) {
        return SessionStatus::failure(
            SessionErrorCode::NotOpen,
            "CRUMBS session is not open");
    }

    const uint32_t timeout_us =
        options_.timeout_ms > (UINT32_MAX / 1000u) ? UINT32_MAX : options_.timeout_ms * 1000u;

    return execute_with_retry(options_, "read", address, [&]() {
        return transport_.read(address, frame, timeout_us);
    });
}

SessionStatus Session::query_read(uint8_t address, uint8_t reply_opcode, RawFrame &out) {
    std::lock_guard<std::mutex> lock(mutex_);

    const SessionStatus address_status = validate_address(address);
    if(!address_status) {
        return address_status;
    }
    if(!transport_.is_open()) {
        return SessionStatus::failure(
            SessionErrorCode::NotOpen,
            "CRUMBS session is not open");
    }

    RawFrame query;
    query.type_id = 0u;
    query.opcode = kSetReplyOpcode;
    query.payload.push_back(reply_opcode);

    const uint32_t timeout_us =
        options_.timeout_ms > (UINT32_MAX / 1000u) ? UINT32_MAX : options_.timeout_ms * 1000u;

    return execute_with_retry(options_, "query_read", address, [&]() {
        SessionStatus status = transport_.send(address, query);
        if(!status) {
            return status;
        }

        transport_.delay_us(options_.query_delay_us);
        return transport_.read(address, out, timeout_us);
    });
}

SessionStatus Session::validate_open_options() const {
    if(options_.bus_path.empty()) {
        return SessionStatus::failure(
            SessionErrorCode::InvalidArgument,
            "session bus_path must not be empty");
    }
    if(options_.retry_count < 0) {
        return SessionStatus::failure(
            SessionErrorCode::InvalidArgument,
            "session retry_count must be non-negative");
    }
    return SessionStatus::success();
}

SessionStatus Session::validate_scan_options(const ScanOptions &options) const {
    if(options.start_address < kMinI2cAddress || options.start_address > kMaxI2cAddress) {
        return SessionStatus::failure(
            SessionErrorCode::InvalidArgument,
            "scan start_address is outside the supported I2C range");
    }
    if(options.end_address < kMinI2cAddress || options.end_address > kMaxI2cAddress) {
        return SessionStatus::failure(
            SessionErrorCode::InvalidArgument,
            "scan end_address is outside the supported I2C range");
    }
    if(options.start_address > options.end_address) {
        return SessionStatus::failure(
            SessionErrorCode::InvalidArgument,
            "scan start_address must be <= end_address");
    }
    if(options.max_results == 0u) {
        return SessionStatus::failure(
            SessionErrorCode::InvalidArgument,
            "scan max_results must be greater than zero");
    }
    return SessionStatus::success();
}

SessionStatus Session::validate_address(uint8_t address) const {
    if(address < kMinI2cAddress || address > kMaxI2cAddress) {
        return SessionStatus::failure(
            SessionErrorCode::InvalidArgument,
            "I2C address is outside the supported range");
    }
    return SessionStatus::success();
}

SessionStatus Session::validate_frame(const RawFrame &frame) const {
    if(frame.payload.size() > kMaxPayloadBytes) {
        return SessionStatus::failure(
            SessionErrorCode::InvalidArgument,
            "CRUMBS payload exceeds 27 byte maximum");
    }
    return SessionStatus::success();
}

SessionOptions make_session_options(const ProviderConfig &config) {
    SessionOptions options;
    options.bus_path = config.bus_path;
    options.query_delay_us = static_cast<uint32_t>(config.query_delay_us);
    options.timeout_ms = static_cast<uint32_t>(config.timeout_ms);
    options.retry_count = config.retry_count;
    return options;
}

bool is_retryable(SessionErrorCode code) {
    switch(code) {
    case SessionErrorCode::ScanFailed:
    case SessionErrorCode::WriteFailed:
    case SessionErrorCode::ReadFailed:
    case SessionErrorCode::Timeout:
    case SessionErrorCode::DecodeFailed:
    case SessionErrorCode::TransportError:
        return true;
    case SessionErrorCode::Ok:
    case SessionErrorCode::InvalidArgument:
    case SessionErrorCode::NotOpen:
    case SessionErrorCode::AlreadyOpen:
    case SessionErrorCode::OpenFailed:
        return false;
    }

    return false;
}

std::string to_string(SessionErrorCode code) {
    switch(code) {
    case SessionErrorCode::Ok:
        return "ok";
    case SessionErrorCode::InvalidArgument:
        return "invalid_argument";
    case SessionErrorCode::NotOpen:
        return "not_open";
    case SessionErrorCode::AlreadyOpen:
        return "already_open";
    case SessionErrorCode::OpenFailed:
        return "open_failed";
    case SessionErrorCode::ScanFailed:
        return "scan_failed";
    case SessionErrorCode::WriteFailed:
        return "write_failed";
    case SessionErrorCode::ReadFailed:
        return "read_failed";
    case SessionErrorCode::Timeout:
        return "timeout";
    case SessionErrorCode::DecodeFailed:
        return "decode_failed";
    case SessionErrorCode::TransportError:
        return "transport_error";
    }

    return "unknown";
}

} // namespace anolis_provider_bread::crumbs