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
#include <linux/i2c.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "crumbs/crumbs_transport.hpp"
#include "crumbs/session.hpp"

namespace {

bool g_intercept = false;
int g_errno = EAGAIN;
long g_fail_first = 0;
long g_rdwr_calls = 0;
long g_rdwr_writes = 0;           // I2C_RDWR calls whose messages carry no read
bool g_fail_writes_only = false;  // g_fail_first counts writes, not every call

}  // namespace

extern "C" int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (g_intercept) {
        if (request == I2C_RDWR) {
            ++g_rdwr_calls;
            const auto *data = static_cast<const i2c_rdwr_ioctl_data *>(arg);
            bool reads = false;
            for (__u32 i = 0; data != nullptr && i < data->nmsgs; ++i) {
                reads = reads || (data->msgs[i].flags & I2C_M_RD) != 0;
            }
            if (!reads) {
                ++g_rdwr_writes;
            }
            // A read gets 0xFF, what an idle bus reads, which is no CRUMBS
            // frame: the scan then always sends its probe write. Left unset,
            // the scan decided from whatever the buffer held, so a clean scan
            // made 1 call in one build and 3 in another (#257).
            for (__u32 i = 0; data != nullptr && i < data->nmsgs; ++i) {
                if ((data->msgs[i].flags & I2C_M_RD) != 0) {
                    std::memset(data->msgs[i].buf, 0xFF, data->msgs[i].len);
                }
            }
            const long counted = g_fail_writes_only ? (reads ? 0 : g_rdwr_writes) : g_rdwr_calls;
            if (counted > 0 && counted <= g_fail_first) {
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
        g_fail_writes_only = false;
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

// Review of #254: a scan's probe writes go around Session, so with the bus at
// no retries a single EAGAIN dropped an address from discovery. They get
// Session's budget back: one busy write costs one more bus call, and a
// persistent one stops after retry_count + 1 writes.
long scan_one_address(const std::string &bus_path) {
    auto transport = make_linux_transport(bus_path);
    Session session(*transport, SessionOptions{bus_path, 10000u, 100u, 2});
    if (!session.open()) {
        return -1;
    }
    g_rdwr_calls = 0;
    g_rdwr_writes = 0;
    ScanOptions options;
    options.start_address = 0x08;
    options.end_address = 0x08;
    std::vector<ScanResult> found;
    (void)session.scan(options, found);
    return g_rdwr_calls;
}

TEST_F(BusRetryTest, AScanProbeRetriesABusyWrite) {
    g_fail_first = 0;
    const long clean = scan_one_address(bus_path_);
    ASSERT_GT(clean, 0);

    // The scan reads before it writes, so the first call is a read: fail the
    // first write, the probe write this test is about (#257).
    g_errno = EAGAIN;
    g_fail_first = 1;
    g_fail_writes_only = true;
    EXPECT_EQ(scan_one_address(bus_path_), clean + 1) << "one EAGAIN on the probe write is retried once";
}

TEST_F(BusRetryTest, AScanProbeRetriesABusyWriteUpToTheBudgetAndAHardErrorNotAtAll) {
    g_fail_first = 1000;
    g_errno = EIO;
    ASSERT_GE(scan_one_address(bus_path_), 0);
    const long hard_writes = g_rdwr_writes;
    ASSERT_GT(hard_writes, 0);

    g_errno = EAGAIN;
    ASSERT_GE(scan_one_address(bus_path_), 0);
    EXPECT_EQ(g_rdwr_writes, 3 * hard_writes)
        << "retry_count 2: each probe write is tried 3 times when busy, once on EIO (" << hard_writes << ")";
}

}  // namespace
}  // namespace anolis_provider_bread::crumbs
