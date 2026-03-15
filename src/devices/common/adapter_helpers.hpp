#pragma once

// Windows macro protection
#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#endif

#include <cstdint>
#include <string>
#include <vector>

#include "protocol.pb.h"
#include <google/protobuf/util/time_util.h>

namespace anolis_provider_bread {

// ---------------------------------------------------------------------------
// Adapter result types (shared by all BREAD hardware adapters)
// ---------------------------------------------------------------------------

struct AdapterReadResult {
    bool ok = false;
    anolis::deviceprovider::v1::Status::Code error_code =
        anolis::deviceprovider::v1::Status::CODE_UNAVAILABLE;
    std::string error_message;
    std::vector<anolis::deviceprovider::v1::SignalValue> values;
};

struct AdapterCallResult {
    bool ok = false;
    anolis::deviceprovider::v1::Status::Code error_code =
        anolis::deviceprovider::v1::Status::CODE_UNAVAILABLE;
    std::string error_message;
};

// ---------------------------------------------------------------------------
// Payload byte building (little-endian)
// ---------------------------------------------------------------------------

inline void append_u8(std::vector<uint8_t> &buf, uint8_t v) {
    buf.push_back(v);
}

inline void append_u16_le(std::vector<uint8_t> &buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFFu));
    buf.push_back(static_cast<uint8_t>(v >> 8u));
}

inline void append_i16_le(std::vector<uint8_t> &buf, int16_t v) {
    append_u16_le(buf, static_cast<uint16_t>(v));
}

// ---------------------------------------------------------------------------
// Payload byte reading (bounds-checked, little-endian)
// ---------------------------------------------------------------------------

inline bool read_u8(const std::vector<uint8_t> &p, std::size_t off, uint8_t &out) {
    if (off >= p.size()) return false;
    out = p[off];
    return true;
}

// Requires p[off] and p[off+1] to be valid. Fails if p.size() < 2 or off+1 >= p.size().
inline bool read_u16_le(const std::vector<uint8_t> &p, std::size_t off, uint16_t &out) {
    if (p.size() < 2u || off > p.size() - 2u) return false;
    out = static_cast<uint16_t>(p[off]) | (static_cast<uint16_t>(p[off + 1u]) << 8u);
    return true;
}

inline bool read_i16_le(const std::vector<uint8_t> &p, std::size_t off, int16_t &out) {
    uint16_t raw = 0u;
    if (!read_u16_le(p, off, raw)) return false;
    out = static_cast<int16_t>(raw);
    return true;
}

// ---------------------------------------------------------------------------
// Arg extraction from protobuf args map
// ---------------------------------------------------------------------------

using ValueMap = google::protobuf::Map<std::string, anolis::deviceprovider::v1::Value>;

inline bool get_arg_bool(const ValueMap &args, const std::string &key, bool &out) {
    const auto it = args.find(key);
    if (it == args.end()) return false;
    if (it->second.type() != anolis::deviceprovider::v1::VALUE_TYPE_BOOL) return false;
    out = it->second.bool_value();
    return true;
}

inline bool get_arg_int64(const ValueMap &args, const std::string &key, int64_t &out) {
    const auto it = args.find(key);
    if (it == args.end()) return false;
    if (it->second.type() != anolis::deviceprovider::v1::VALUE_TYPE_INT64) return false;
    out = it->second.int64_value();
    return true;
}

inline bool get_arg_uint64(const ValueMap &args, const std::string &key, uint64_t &out) {
    const auto it = args.find(key);
    if (it == args.end()) return false;
    if (it->second.type() != anolis::deviceprovider::v1::VALUE_TYPE_UINT64) return false;
    out = it->second.uint64_value();
    return true;
}

inline bool get_arg_double(const ValueMap &args, const std::string &key, double &out) {
    const auto it = args.find(key);
    if (it == args.end()) return false;
    if (it->second.type() != anolis::deviceprovider::v1::VALUE_TYPE_DOUBLE) return false;
    out = it->second.double_value();
    return true;
}

inline bool get_arg_string(const ValueMap &args, const std::string &key, std::string &out) {
    const auto it = args.find(key);
    if (it == args.end()) return false;
    if (it->second.type() != anolis::deviceprovider::v1::VALUE_TYPE_STRING) return false;
    out = it->second.string_value();
    return true;
}

// ---------------------------------------------------------------------------
// Value building helpers
// ---------------------------------------------------------------------------

inline anolis::deviceprovider::v1::Value make_bool_val(bool b) {
    anolis::deviceprovider::v1::Value v;
    v.set_type(anolis::deviceprovider::v1::VALUE_TYPE_BOOL);
    v.set_bool_value(b);
    return v;
}

inline anolis::deviceprovider::v1::Value make_int64_val(int64_t i) {
    anolis::deviceprovider::v1::Value v;
    v.set_type(anolis::deviceprovider::v1::VALUE_TYPE_INT64);
    v.set_int64_value(i);
    return v;
}

inline anolis::deviceprovider::v1::Value make_uint64_val(uint64_t u) {
    anolis::deviceprovider::v1::Value v;
    v.set_type(anolis::deviceprovider::v1::VALUE_TYPE_UINT64);
    v.set_uint64_value(u);
    return v;
}

inline anolis::deviceprovider::v1::Value make_double_val(double d) {
    anolis::deviceprovider::v1::Value v;
    v.set_type(anolis::deviceprovider::v1::VALUE_TYPE_DOUBLE);
    v.set_double_value(d);
    return v;
}

inline anolis::deviceprovider::v1::Value make_string_val(const std::string &s) {
    anolis::deviceprovider::v1::Value v;
    v.set_type(anolis::deviceprovider::v1::VALUE_TYPE_STRING);
    v.set_string_value(s);
    return v;
}

inline anolis::deviceprovider::v1::SignalValue make_signal_value(
    const std::string &id,
    const anolis::deviceprovider::v1::Value &val) {
    anolis::deviceprovider::v1::SignalValue sv;
    sv.set_signal_id(id);
    *sv.mutable_value() = val;
    *sv.mutable_timestamp() = (google::protobuf::util::TimeUtil::GetCurrentTime)();
    sv.set_quality(anolis::deviceprovider::v1::SignalValue::QUALITY_OK);
    return sv;
}

} // namespace anolis_provider_bread
