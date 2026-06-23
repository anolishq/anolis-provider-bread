#pragma once

#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

namespace anolis_provider_bread::transport {

constexpr uint32_t kMaxFrameBytes = 1024u * 1024u;

bool read_exact(std::istream &input, uint8_t *buffer, size_t size);
bool read_frame(std::istream &input, std::vector<uint8_t> &out, std::string &error, uint32_t max_len = kMaxFrameBytes);
bool write_frame(std::ostream &output, const uint8_t *data, size_t size, std::string &error,
                 uint32_t max_len = kMaxFrameBytes);

}  // namespace anolis_provider_bread::transport
