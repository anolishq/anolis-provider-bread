#include "core/handlers.hpp"

#include <gtest/gtest.h>

namespace anolis_provider_bread {
namespace {

using SignalValue = anolis::deviceprovider::v1::SignalValue;

anolis::deviceprovider::v1::ReadSignalsResponse make_response(int64_t value_seconds) {
    anolis::deviceprovider::v1::ReadSignalsResponse out;
    auto *v = out.add_values();
    v->set_signal_id("t1_c");
    v->mutable_timestamp()->set_seconds(value_seconds);
    v->set_quality(SignalValue::QUALITY_OK);
    return out;
}

anolis::deviceprovider::v1::ReadSignalsRequest make_request(int64_t min_seconds, bool has_min) {
    anolis::deviceprovider::v1::ReadSignalsRequest req;
    req.set_device_id("rlht0");
    if (has_min) {
        req.mutable_min_timestamp()->set_seconds(min_seconds);
    }
    return req;
}

// [§7.3] Without a min_timestamp, quality is untouched.
TEST(ApplyMinTimestampTest, NoMinTimestampLeavesQualityUnchanged) {
    auto out = make_response(1000);
    handlers::apply_min_timestamp(make_request(0, false), out);
    EXPECT_EQ(out.values(0).quality(), SignalValue::QUALITY_OK);
}

// A value at/after min_timestamp satisfies the freshness request (live read).
TEST(ApplyMinTimestampTest, PastMinTimestampStaysOk) {
    auto out = make_response(1000);
    handlers::apply_min_timestamp(make_request(500, true), out);
    EXPECT_EQ(out.values(0).quality(), SignalValue::QUALITY_OK);
}

// A value older than min_timestamp (e.g. an unsatisfiable future timestamp) is
// flagged stale rather than reported fresh.
TEST(ApplyMinTimestampTest, FutureMinTimestampFlaggedStale) {
    auto out = make_response(1000);
    handlers::apply_min_timestamp(make_request(2000, true), out);
    EXPECT_EQ(out.values(0).quality(), SignalValue::QUALITY_STALE);
}

}  // namespace
}  // namespace anolis_provider_bread
