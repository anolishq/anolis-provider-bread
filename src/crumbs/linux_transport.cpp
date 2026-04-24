#include "crumbs/linux_transport.hpp"

/**
 * @file linux_transport.cpp
 * @brief Linux I2C transport implementation for the generic CRUMBS session
 * layer.
 */

#include <cerrno>
#include <cstring>
#include <sstream>

namespace anolis_provider_bread::crumbs {
namespace {

SessionStatus failure_from_errno(SessionErrorCode code,
                                 const std::string &message, int native_code,
                                 int saved_errno) {
  std::ostringstream out;
  out << message;
  if (native_code != 0) {
    out << " (native_code=" << native_code << ")";
  }
  if (saved_errno != 0) {
    out << ": " << std::strerror(saved_errno);
  }
  return SessionStatus::failure(code, out.str(), native_code);
}

uint32_t timeout_ms_to_us(uint32_t timeout_ms) {
  if (timeout_ms > (UINT32_MAX / 1000u)) {
    return UINT32_MAX;
  }
  return timeout_ms * 1000u;
}

SessionStatus validate_frame(const RawFrame &frame) {
  if (frame.payload.size() > CRUMBS_MAX_PAYLOAD) {
    return SessionStatus::failure(SessionErrorCode::InvalidArgument,
                                  "CRUMBS payload exceeds 27 byte maximum");
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

} // namespace

SessionStatus LinuxTransport::open(const SessionOptions &options) {
  if (options.bus_path.empty()) {
    return SessionStatus::failure(SessionErrorCode::InvalidArgument,
                                  "session bus_path must not be empty");
  }
  if (open_) {
    return SessionStatus::failure(SessionErrorCode::AlreadyOpen,
                                  "CRUMBS Linux transport is already open");
  }

  errno = 0;
  // The transport stores the bus-level timeout once at open time so later
  // scan/send/read operations share the same controller configuration.
  const uint32_t timeout_us = timeout_ms_to_us(options.timeout_ms);
  const int rc = crumbs_linux_init_controller(
      &ctx_, &i2c_, options.bus_path.c_str(), timeout_us);
  const int saved_errno = errno;
  if (rc != 0) {
    return failure_from_errno(SessionErrorCode::OpenFailed,
                              "failed to open Linux I2C bus '" +
                                  options.bus_path + "'",
                              rc, saved_errno);
  }

  timeout_us_ = timeout_us;
  open_ = true;
  return SessionStatus::success();
}

void LinuxTransport::close() noexcept {
  if (!open_) {
    return;
  }

  crumbs_linux_close(&i2c_);
  timeout_us_ = 0u;
  open_ = false;
}

bool LinuxTransport::is_open() const { return open_; }

SessionStatus LinuxTransport::scan(const ScanOptions &options,
                                   std::vector<ScanResult> &out) {
  if (!open_) {
    return SessionStatus::failure(SessionErrorCode::NotOpen,
                                  "CRUMBS Linux transport is not open");
  }

  std::vector<uint8_t> addresses(options.max_results, 0u);
  std::vector<uint8_t> types(options.max_results, 0u);

  errno = 0;
  // Scanning remains a transport responsibility; higher layers interpret the
  // resulting type IDs and compatibility details when building inventory.
  const int rc = crumbs_linux_scan_for_crumbs_with_types(
      &ctx_, &i2c_, options.start_address, options.end_address,
      options.strict ? 1 : 0, addresses.data(), types.data(),
      options.max_results, timeout_us_);
  const int saved_errno = errno;
  if (rc < 0) {
    return failure_from_errno(SessionErrorCode::ScanFailed,
                              "failed to scan CRUMBS devices", rc, saved_errno);
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

SessionStatus LinuxTransport::send(uint8_t address, const RawFrame &frame) {
  if (!open_) {
    return SessionStatus::failure(SessionErrorCode::NotOpen,
                                  "CRUMBS Linux transport is not open");
  }

  const SessionStatus validation = validate_frame(frame);
  if (!validation) {
    return validation;
  }

  crumbs_message_t message{};
  populate_message(frame, message);

  errno = 0;
  const int rc = crumbs_controller_send(&ctx_, address, &message,
                                        crumbs_linux_i2c_write, &i2c_);
  const int saved_errno = errno;
  if (rc != 0) {
    return failure_from_errno(SessionErrorCode::WriteFailed,
                              "failed to send CRUMBS frame to " +
                                  format_i2c_address(static_cast<int>(address)),
                              rc, saved_errno);
  }

  return SessionStatus::success();
}

SessionStatus LinuxTransport::read(uint8_t address, RawFrame &frame,
                                   uint32_t timeout_us) {
  if (!open_) {
    return SessionStatus::failure(SessionErrorCode::NotOpen,
                                  "CRUMBS Linux transport is not open");
  }

  uint8_t buffer[CRUMBS_MESSAGE_MAX_SIZE] = {0};

  errno = 0;
  const int bytes_read =
      crumbs_linux_read(&i2c_, address, buffer, sizeof(buffer), timeout_us);
  const int saved_errno = errno;
  if (bytes_read < 0) {
    const SessionErrorCode code = saved_errno == ETIMEDOUT
                                      ? SessionErrorCode::Timeout
                                      : SessionErrorCode::ReadFailed;
    return failure_from_errno(code,
                              "failed to read CRUMBS frame from " +
                                  format_i2c_address(static_cast<int>(address)),
                              bytes_read, saved_errno);
  }
  if (bytes_read < 4) {
    return SessionStatus::failure(
        SessionErrorCode::ReadFailed,
        "short CRUMBS frame read from " +
            format_i2c_address(static_cast<int>(address)),
        bytes_read);
  }

  crumbs_message_t message{};
  // Decode failures are surfaced distinctly because adapter code assumes a
  // valid BREAD payload shape once a frame reaches it.
  const int rc = crumbs_decode_message(
      buffer, static_cast<std::size_t>(bytes_read), &message, &ctx_);
  if (rc != 0) {
    const SessionErrorCode code = rc == -2 ? SessionErrorCode::DecodeFailed
                                           : SessionErrorCode::ReadFailed;
    return SessionStatus::failure(
        code,
        "failed to decode CRUMBS reply from " +
            format_i2c_address(static_cast<int>(address)),
        rc);
  }

  frame = to_raw_frame(message);
  return SessionStatus::success();
}

void LinuxTransport::delay_us(uint32_t delay_us) {
  crumbs_linux_delay_us(delay_us);
}

crumbs_device_t LinuxTransport::bind_device(uint8_t address) {
  crumbs_device_t device{};
  // This handle is intentionally non-owning: the surrounding Session keeps
  // the controller and transport alive for as long as BREAD helper calls run.
  device.ctx = &ctx_;
  device.addr = address;
  device.write_fn = crumbs_linux_i2c_write;
  device.read_fn = crumbs_linux_read;
  device.delay_fn = crumbs_linux_delay_us;
  device.io = &i2c_;
  return device;
}

} // namespace anolis_provider_bread::crumbs
