#include "crumbs/crumbs_transport.hpp"

/**
 * @file crumbs_transport.cpp
 * @brief CRUMBS-over-I2cBus transport: raw I/O on the shared bus, CRUMBS framing
 * and scan via the controller helpers.
 */

#include <cstring>
#include <utility>

namespace anolis_provider_bread::crumbs {
namespace {

namespace sdk_i2c = anolis::provider_sdk::i2c;

// Floor for the per-address scan-probe deadline (see open()).
constexpr uint32_t kMinScanProbeTimeoutUs = 2000u;

// Choose the per-address deadline the scan hands each probe read.
//
// The shared LinuxI2cBus::read polls to its deadline on a silent device, and a
// non-strict full-range scan probes ~112 mostly-absent addresses (twice each).
// Using the 100ms request timeout would block startup discovery for ~20s. A
// present CRUMBS device stages its reply within the query-delay window, so scan
// probes use that (short) window instead — detection is at least as reliable as
// the old fast-NAK path (which waited even less), while absent addresses fail in
// milliseconds. The per-transaction request timeout still governs live reads.
uint32_t scan_probe_timeout_us(const SessionOptions &options) {
    return options.query_delay_us > kMinScanProbeTimeoutUs ? options.query_delay_us : kMinScanProbeTimeoutUs;
}

SessionStatus validate_frame(const RawFrame &frame) {
    if (frame.payload.size() > CRUMBS_MAX_PAYLOAD) {
        return SessionStatus::failure(SessionErrorCode::InvalidArgument, "CRUMBS payload exceeds 27 byte maximum");
    }
    return SessionStatus::success();
}

void populate_message(const RawFrame &frame, crumbs_message_t &message) {
    message.type_id = frame.type_id;
    message.opcode = frame.opcode;
    message.data_len = static_cast<uint8_t>(frame.payload.size());
    if (!frame.payload.empty()) {
        std::memcpy(message.data, frame.payload.data(), frame.payload.size());
    }
}

RawFrame to_raw_frame(const crumbs_message_t &message) {
    RawFrame frame;
    frame.type_id = message.type_id;
    frame.opcode = message.opcode;
    frame.payload.assign(message.data, message.data + message.data_len);
    return frame;
}

// Map a transport I2cStatus onto the CRUMBS SessionStatus, preserving the
// message and native errno the bus captured.
SessionStatus status_from_i2c(SessionErrorCode code, const std::string &context, const sdk_i2c::I2cStatus &status) {
    std::string message = context;
    if (!status.message.empty()) {
        message += ": " + status.message;
    }
    return SessionStatus::failure(code, std::move(message), status.native_errno);
}

// CRUMBS controller-helper callbacks: user_ctx is the I2cBus.
int bus_write_fn(void *user_ctx, uint8_t addr, const uint8_t *data, size_t len) {
    auto *bus = static_cast<sdk_i2c::I2cBus *>(user_ctx);
    return bus->write(addr, data, len) ? 0 : -1;
}

int bus_read_fn(void *user_ctx, uint8_t addr, uint8_t *buffer, size_t len, uint32_t timeout_us) {
    auto *bus = static_cast<sdk_i2c::I2cBus *>(user_ctx);
    size_t received = 0;
    if (!bus->read(addr, buffer, len, &received, timeout_us)) {
        return -1;
    }
    return static_cast<int>(received);
}

}  // namespace

CrumbsTransport::CrumbsTransport(std::unique_ptr<sdk_i2c::I2cBus> bus) : bus_(std::move(bus)) {}

SessionStatus CrumbsTransport::open(const SessionOptions &options) {
    if (options.bus_path.empty()) {
        return SessionStatus::failure(SessionErrorCode::InvalidArgument, "session bus_path must not be empty");
    }
    if (open_) {
        return SessionStatus::failure(SessionErrorCode::AlreadyOpen, "CRUMBS transport is already open");
    }
    if (bus_ == nullptr) {
        return SessionStatus::failure(SessionErrorCode::OpenFailed, "CRUMBS transport has no bus");
    }

    const sdk_i2c::I2cStatus status = bus_->open();
    if (!status) {
        return status_from_i2c(SessionErrorCode::OpenFailed, "failed to open I2C bus '" + options.bus_path + "'",
                               status);
    }

    // Controller-role CRUMBS context for encode/decode CRC accounting. No
    // hardware setup here — the bus owns the platform mechanics.
    crumbs_init(&ctx_, CRUMBS_ROLE_CONTROLLER, 0);
    scan_timeout_us_ = scan_probe_timeout_us(options);
    open_ = true;
    return SessionStatus::success();
}

void CrumbsTransport::close() noexcept {
    if (!open_) {
        return;
    }
    if (bus_ != nullptr) {
        bus_->close();
    }
    scan_timeout_us_ = 0u;
    open_ = false;
}

bool CrumbsTransport::is_open() const { return open_; }

SessionStatus CrumbsTransport::scan(const ScanOptions &options, std::vector<ScanResult> &out) {
    if (!open_) {
        return SessionStatus::failure(SessionErrorCode::NotOpen, "CRUMBS transport is not open");
    }

    std::vector<uint8_t> addresses(options.max_results, 0u);
    std::vector<uint8_t> types(options.max_results, 0u);

    // The CRUMBS controller scan drives probe writes + reads through the bus
    // callbacks and validates each responder via CRC decode, returning type IDs.
    const int rc = crumbs_controller_scan_for_crumbs_with_types(
        &ctx_, options.start_address, options.end_address, options.strict ? 1 : 0, bus_write_fn, bus_read_fn,
        bus_.get(), addresses.data(), types.data(), options.max_results, scan_timeout_us_);
    if (rc < 0) {
        return SessionStatus::failure(SessionErrorCode::ScanFailed, "failed to scan CRUMBS devices", rc);
    }

    out.clear();
    out.reserve(static_cast<std::size_t>(rc));
    for (int i = 0; i < rc; ++i) {
        ScanResult result;
        result.address = addresses[static_cast<std::size_t>(i)];
        result.has_type_id = true;
        result.type_id = types[static_cast<std::size_t>(i)];
        out.push_back(result);
    }
    return SessionStatus::success();
}

SessionStatus CrumbsTransport::send(uint8_t address, const RawFrame &frame) {
    if (!open_) {
        return SessionStatus::failure(SessionErrorCode::NotOpen, "CRUMBS transport is not open");
    }

    SessionStatus validation = validate_frame(frame);
    if (!validation) {
        return validation;
    }

    crumbs_message_t message{};
    populate_message(frame, message);

    uint8_t buffer[CRUMBS_MESSAGE_MAX_SIZE] = {0};
    const size_t encoded = crumbs_encode_message(&message, buffer, sizeof(buffer));
    if (encoded == 0) {
        return SessionStatus::failure(SessionErrorCode::WriteFailed, "failed to encode CRUMBS frame for " +
                                                                         format_i2c_address(static_cast<int>(address)));
    }

    const sdk_i2c::I2cStatus status = bus_->write(address, buffer, encoded);
    if (!status) {
        return status_from_i2c(SessionErrorCode::WriteFailed,
                               "failed to send CRUMBS frame to " + format_i2c_address(static_cast<int>(address)),
                               status);
    }
    return SessionStatus::success();
}

SessionStatus CrumbsTransport::read(uint8_t address, RawFrame &frame, uint32_t timeout_us) {
    if (!open_) {
        return SessionStatus::failure(SessionErrorCode::NotOpen, "CRUMBS transport is not open");
    }

    uint8_t buffer[CRUMBS_MESSAGE_MAX_SIZE] = {0};
    size_t received = 0;
    const sdk_i2c::I2cStatus status = bus_->read(address, buffer, sizeof(buffer), &received, timeout_us);
    if (!status) {
        const SessionErrorCode code =
            status.code == sdk_i2c::I2cError::Timeout ? SessionErrorCode::Timeout : SessionErrorCode::ReadFailed;
        return status_from_i2c(
            code, "failed to read CRUMBS frame from " + format_i2c_address(static_cast<int>(address)), status);
    }

    // A fixed-count read returns bus padding after the frame; trim to the
    // header-declared length before the exact-length decode (crumbs_frame_length
    // rejects short reads and garbage headers). This is the bread#97 surface.
    std::size_t frame_len = 0;
    if (crumbs_frame_length(buffer, received, &frame_len) != 0) {
        return SessionStatus::failure(
            SessionErrorCode::ReadFailed,
            "invalid or truncated CRUMBS frame from " + format_i2c_address(static_cast<int>(address)),
            static_cast<int>(received));
    }

    crumbs_message_t message{};
    const int rc = crumbs_decode_message(buffer, frame_len, &message, &ctx_);
    if (rc != 0) {
        const SessionErrorCode code = rc == -2 ? SessionErrorCode::DecodeFailed : SessionErrorCode::ReadFailed;
        return SessionStatus::failure(
            code, "failed to decode CRUMBS reply from " + format_i2c_address(static_cast<int>(address)), rc);
    }

    frame = to_raw_frame(message);
    return SessionStatus::success();
}

void CrumbsTransport::delay_us(uint32_t delay_us) {
    if (bus_ != nullptr) {
        bus_->delay_us(delay_us);
    }
}

}  // namespace anolis_provider_bread::crumbs
