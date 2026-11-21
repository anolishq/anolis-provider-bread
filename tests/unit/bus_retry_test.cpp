/**
 * @file bus_retry_test.cpp
 * @brief One retry layer between a CRUMBS operation and the Linux I2C bus.
 *
 * Drives the real transport `make_linux_transport` builds (the SDK's
 * `LinuxI2cBus` under `CrumbsTransport`) through a `Session`, with `ioctl`
 * replaced in this executable so `I2C_RDWR` fails with a chosen errno for the
 * first N calls. The bus path is a regular file, so the bus opens without
 * hardware. Built as its own executable because the `ioctl` replacement covers
 * the whole process (anolis-provider-bread#198).
 */

#include <dlfcn.h>
#include <gtest/gtest.h>
#include <linux/i2c-dev.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "crumbs/crumbs_transport.hpp"
#include "crumbs/session.hpp"

namespace {

bool g_intercept = false;
int g_errno = EAGAIN;
long g_fail_first = 0;
long g_rdwr_calls = 0;

}  // namespace

extern "C" int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (g_intercept) {
        if (request == I2C_RDWR) {
            ++g_rdwr_calls;
            if (g_rdwr_calls <= g_fail_first) {
                errno = g_errno;
                return -1;
            }
            return 1;
        }
        if (request == I2C_SLAVE || request == I2C_SLAVE_FORCE || request == I2C_FUNCS) {
            return 0;
        }
    }
    using RealIoctl = int (*)(int, unsigned long, ...);
    static const auto real = reinterpret_cast<RealIoctl>(dlsym(RTLD_NEXT, "ioctl"));
    return real(fd, request, arg);
}

namespace anolis_provider_bread::crumbs {
namespace {

class BusRetryTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char *dir = std::getenv("TMPDIR");
        std::string templ = std::string(dir != nullptr && *dir != '\0' ? dir : "/tmp") + "/bread-bus-retry-XXXXXX";
        const int fd = mkstemp(templ.data());
        ASSERT_GE(fd, 0) << "mkstemp failed for " << templ;
        ::close(fd);
        bus_path_ = templ;
        g_rdwr_calls = 0;
        g_intercept = true;
    }

    void TearDown() override {
        g_intercept = false;
        std::remove(bus_path_.c_str());
    }

    // One send at 0x08 through a session with `retry_count` 2, the default.
    SessionStatus send_once(Session &session) {
        RawFrame frame;
        frame.type_id = 0x01;
        frame.opcode = 0x10;
        return session.send(0x08, frame);
    }

    std::string bus_path_;
};

TEST_F(BusRetryTest, APersistentEagainMakesOneBusWritePerSessionAttempt) {
    g_errno = EAGAIN;
    g_fail_first = 1000;
    auto transport = make_linux_transport(bus_path_);
    Session session(*transport, SessionOptions{bus_path_, 10000u, 100u, 2});
    ASSERT_TRUE(session.open());

    EXPECT_FALSE(send_once(session).ok());

    EXPECT_EQ(g_rdwr_calls, 3) << "retry_count 2 allows 3 bus writes per operation";
    const AddressStats stats = session.stats_for(0x08);
    EXPECT_EQ(stats.failed, 1U);
    EXPECT_EQ(stats.retried_attempts, 2U);
}

TEST_F(BusRetryTest, EagainAbsorbedByRetriesStillCountsTheRetriedAttempts) {
    g_errno = EAGAIN;
    g_fail_first = 2;
    auto transport = make_linux_transport(bus_path_);
    Session session(*transport, SessionOptions{bus_path_, 10000u, 100u, 2});
    ASSERT_TRUE(session.open());

    EXPECT_TRUE(send_once(session).ok());

    EXPECT_EQ(g_rdwr_calls, 3);
    const AddressStats stats = session.stats_for(0x08);
    EXPECT_EQ(stats.ok, 1U);
    EXPECT_EQ(stats.retried_attempts, 2U) << "the bus's absorbed EAGAINs must show in io_retried_attempts";
}

}  // namespace
}  // namespace anolis_provider_bread::crumbs
