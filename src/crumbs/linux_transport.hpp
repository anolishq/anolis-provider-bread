#pragma once

#include "crumbs/session.hpp"

#if defined(ANOLIS_PROVIDER_BREAD_HAS_CRUMBS)

extern "C" {
#include "crumbs.h"
#include "crumbs_linux.h"
}

namespace anolis_provider_bread::crumbs {

class LinuxTransport final : public Transport {
public:
    SessionStatus open(const SessionOptions &options) override;
    void close() noexcept override;
    bool is_open() const override;
    SessionStatus scan(const ScanOptions &options, std::vector<ScanResult> &out) override;
    SessionStatus send(uint8_t address, const RawFrame &frame) override;
    SessionStatus read(uint8_t address, RawFrame &frame, uint32_t timeout_us) override;
    void delay_us(uint32_t delay_us) override;

    crumbs_device_t bind_device(uint8_t address);

private:
    crumbs_context_t ctx_{};
    crumbs_linux_i2c_t i2c_{};
    uint32_t timeout_us_ = 0u;
    bool open_ = false;
};

} // namespace anolis_provider_bread::crumbs

#endif