#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "protocol.pb.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

uint32_t decode_u32_le(const std::array<uint8_t, 4> &bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

std::array<uint8_t, 4> encode_u32_le(uint32_t value) {
    return {static_cast<uint8_t>(value & 0xFFu),
            static_cast<uint8_t>((value >> 8) & 0xFFu),
            static_cast<uint8_t>((value >> 16) & 0xFFu),
            static_cast<uint8_t>((value >> 24) & 0xFFu)};
}

class ProviderSession {
public:
    ProviderSession(const std::string &exe_path, const std::string &config_path)
        : exe_path_(exe_path),
          config_path_(config_path) {
#ifdef _WIN32
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;

        HANDLE child_stdin_read = nullptr;
        HANDLE parent_stdin_write = nullptr;
        HANDLE parent_stdout_read = nullptr;
        HANDLE child_stdout_write = nullptr;

        if(!CreatePipe(&child_stdin_read, &parent_stdin_write, &sa, 0)) {
            throw std::runtime_error(last_error_message("CreatePipe(stdin)"));
        }
        if(!SetHandleInformation(parent_stdin_write, HANDLE_FLAG_INHERIT, 0)) {
            throw std::runtime_error(last_error_message("SetHandleInformation(stdin)"));
        }

        if(!CreatePipe(&parent_stdout_read, &child_stdout_write, &sa, 0)) {
            CloseHandle(child_stdin_read);
            CloseHandle(parent_stdin_write);
            throw std::runtime_error(last_error_message("CreatePipe(stdout)"));
        }
        if(!SetHandleInformation(parent_stdout_read, HANDLE_FLAG_INHERIT, 0)) {
            CloseHandle(child_stdin_read);
            CloseHandle(parent_stdin_write);
            CloseHandle(parent_stdout_read);
            CloseHandle(child_stdout_write);
            throw std::runtime_error(last_error_message("SetHandleInformation(stdout)"));
        }

        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = child_stdin_read;
        startup.hStdOutput = child_stdout_write;
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);

        PROCESS_INFORMATION process{};
        const std::string abs_path = std::filesystem::absolute(exe_path_).string();
        std::string command = "\"" + abs_path + "\" --config \"" + config_path_ + "\"";
        std::vector<char> command_line(command.begin(), command.end());
        command_line.push_back('\0');

        if(!CreateProcessA(nullptr,
                           command_line.data(),
                           nullptr,
                           nullptr,
                           TRUE,
                           0,
                           nullptr,
                           nullptr,
                           &startup,
                           &process)) {
            CloseHandle(child_stdin_read);
            CloseHandle(parent_stdin_write);
            CloseHandle(parent_stdout_read);
            CloseHandle(child_stdout_write);
            throw std::runtime_error(last_error_message("CreateProcessA"));
        }

        CloseHandle(child_stdin_read);
        CloseHandle(child_stdout_write);
        CloseHandle(process.hThread);

        process_handle_ = process.hProcess;
        stdin_write_ = parent_stdin_write;
        stdout_read_ = parent_stdout_read;
#else
        int stdin_pipe[2] = {-1, -1};
        int stdout_pipe[2] = {-1, -1};
        if(pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
            throw std::runtime_error("pipe failed");
        }

        pid_ = fork();
        if(pid_ < 0) {
            throw std::runtime_error("fork failed");
        }
        if(pid_ == 0) {
            dup2(stdin_pipe[0], STDIN_FILENO);
            dup2(stdout_pipe[1], STDOUT_FILENO);
            ::close(stdin_pipe[1]);
            ::close(stdout_pipe[0]);
            const std::string abs_path = std::filesystem::absolute(exe_path_).string();
            execl(abs_path.c_str(), abs_path.c_str(), "--config", config_path_.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }

        ::close(stdin_pipe[0]);
        ::close(stdout_pipe[1]);

        stdin_fd_ = stdin_pipe[1];
        stdout_fd_ = stdout_pipe[0];
#endif
    }

    ~ProviderSession() {
        close();
    }

    anolis::deviceprovider::v1::Response send(const anolis::deviceprovider::v1::Request &request) {
        std::string payload;
        if(!request.SerializeToString(&payload)) {
            throw std::runtime_error("failed to serialize request");
        }

        const auto header = encode_u32_le(static_cast<uint32_t>(payload.size()));
        write_all(header.data(), header.size());
        write_all(reinterpret_cast<const uint8_t *>(payload.data()), payload.size());

        std::array<uint8_t, 4> response_header{};
        read_exact(response_header.data(), response_header.size());
        const uint32_t response_size = decode_u32_le(response_header);
        std::vector<uint8_t> response_payload(response_size);
        read_exact(response_payload.data(), response_payload.size());

        anolis::deviceprovider::v1::Response response;
        if(!response.ParseFromArray(response_payload.data(), static_cast<int>(response_payload.size()))) {
            throw std::runtime_error("failed to parse response");
        }
        return response;
    }

    void close() {
        if(closed_) {
            return;
        }
        closed_ = true;
#ifdef _WIN32
        if(stdin_write_) {
            CloseHandle(stdin_write_);
            stdin_write_ = nullptr;
        }
        if(process_handle_) {
            WaitForSingleObject(process_handle_, 2000);
            CloseHandle(process_handle_);
            process_handle_ = nullptr;
        }
        if(stdout_read_) {
            CloseHandle(stdout_read_);
            stdout_read_ = nullptr;
        }
#else
        if(stdin_fd_ >= 0) {
            ::close(stdin_fd_);
            stdin_fd_ = -1;
        }
        if(pid_ > 0) {
            int status = 0;
            waitpid(pid_, &status, 0);
            pid_ = -1;
        }
        if(stdout_fd_ >= 0) {
            ::close(stdout_fd_);
            stdout_fd_ = -1;
        }
#endif
    }

private:
#ifdef _WIN32
    static std::string last_error_message(const char *context) {
        const DWORD error = GetLastError();
        LPSTR buffer = nullptr;
        const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                            FORMAT_MESSAGE_FROM_SYSTEM |
                            FORMAT_MESSAGE_IGNORE_INSERTS;
        FormatMessageA(flags,
                       nullptr,
                       error,
                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       reinterpret_cast<LPSTR>(&buffer),
                       0,
                       nullptr);

        std::string message = context;
        message += " failed";
        message += " (error=" + std::to_string(error) + ")";
        if(buffer) {
            message += ": ";
            message += buffer;
            LocalFree(buffer);
        }
        return message;
    }

    [[noreturn]] void throw_io_failure(const char *operation) {
        std::string message = last_error_message(operation);
        if(process_handle_) {
            WaitForSingleObject(process_handle_, 200);
            DWORD exit_code = STILL_ACTIVE;
            if(GetExitCodeProcess(process_handle_, &exit_code)) {
                message += "; exit_code=" + std::to_string(exit_code);
            }
        }
        throw std::runtime_error(message);
    }
#endif

    void write_all(const uint8_t *data, size_t size) {
#ifdef _WIN32
        DWORD written = 0;
        size_t offset = 0;
        while(offset < size) {
            if(!WriteFile(stdin_write_, data + offset, static_cast<DWORD>(size - offset), &written, nullptr)) {
                throw_io_failure("WriteFile");
            }
            if(written == 0) {
                throw std::runtime_error("WriteFile wrote 0 bytes");
            }
            offset += written;
        }
#else
        size_t offset = 0;
        while(offset < size) {
            const ssize_t written = ::write(stdin_fd_, data + offset, size - offset);
            if(written <= 0) {
                throw std::runtime_error("write failed");
            }
            offset += static_cast<size_t>(written);
        }
#endif
    }

    void read_exact(uint8_t *data, size_t size) {
#ifdef _WIN32
        DWORD read = 0;
        size_t offset = 0;
        while(offset < size) {
            if(!ReadFile(stdout_read_, data + offset, static_cast<DWORD>(size - offset), &read, nullptr)) {
                throw_io_failure("ReadFile");
            }
            if(read == 0) {
                throw std::runtime_error("ReadFile returned EOF");
            }
            offset += read;
        }
#else
        size_t offset = 0;
        while(offset < size) {
            const ssize_t read_count = ::read(stdout_fd_, data + offset, size - offset);
            if(read_count <= 0) {
                throw std::runtime_error("read failed");
            }
            offset += static_cast<size_t>(read_count);
        }
#endif
    }

    std::string exe_path_;
    std::string config_path_;
    bool closed_ = false;
#ifdef _WIN32
    HANDLE process_handle_ = nullptr;
    HANDLE stdin_write_ = nullptr;
    HANDLE stdout_read_ = nullptr;
#else
    pid_t pid_ = -1;
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
#endif
};

TEST(ShellTest, SupportsHelloInventoryAndHealth) {
    ProviderSession session(ANOLIS_PROVIDER_BREAD_TEST_EXE, ANOLIS_PROVIDER_BREAD_TEST_CONFIG);

    {
        SCOPED_TRACE("hello");
        anolis::deviceprovider::v1::Request hello;
        hello.set_request_id(1);
        hello.mutable_hello()->set_protocol_version("v1");
        hello.mutable_hello()->set_client_name("shell-test");
        hello.mutable_hello()->set_client_version("0.1.0");
        const auto response = session.send(hello);
        ASSERT_EQ(response.status().code(), anolis::deviceprovider::v1::Status::CODE_OK);
        EXPECT_EQ(response.hello().provider_name(), "anolis-provider-bread");
        EXPECT_EQ(response.hello().protocol_version(), "v1");
    }

    {
        SCOPED_TRACE("wait_ready");
        anolis::deviceprovider::v1::Request wait_ready;
        wait_ready.set_request_id(2);
        wait_ready.mutable_wait_ready()->set_max_wait_ms_hint(5000);
        const auto response = session.send(wait_ready);
        ASSERT_EQ(response.status().code(), anolis::deviceprovider::v1::Status::CODE_OK);
        EXPECT_EQ(response.wait_ready().diagnostics().at("inventory_mode"), "config_seeded");
        EXPECT_EQ(response.wait_ready().diagnostics().at("device_count"), "2");
    }

    {
        SCOPED_TRACE("list_devices");
        anolis::deviceprovider::v1::Request list_devices;
        list_devices.set_request_id(3);
        list_devices.mutable_list_devices()->set_include_health(true);
        const auto response = session.send(list_devices);
        ASSERT_EQ(response.status().code(), anolis::deviceprovider::v1::Status::CODE_OK);
        ASSERT_EQ(response.list_devices().devices_size(), 2);
        ASSERT_EQ(response.list_devices().device_health_size(), 2);
        EXPECT_EQ(response.list_devices().devices(0).provider_name(), "bread-lab");
    }

    {
        SCOPED_TRACE("describe_device");
        anolis::deviceprovider::v1::Request describe;
        describe.set_request_id(4);
        describe.mutable_describe_device()->set_device_id("rlht0");
        const auto response = session.send(describe);
        ASSERT_EQ(response.status().code(), anolis::deviceprovider::v1::Status::CODE_OK);
        EXPECT_EQ(response.describe_device().device().type_id(), "bread.rlht");
        EXPECT_EQ(response.describe_device().capabilities().functions_size(), 6);
    }

    {
        SCOPED_TRACE("get_health");
        anolis::deviceprovider::v1::Request get_health;
        get_health.set_request_id(5);
        get_health.mutable_get_health();
        const auto response = session.send(get_health);
        ASSERT_EQ(response.status().code(), anolis::deviceprovider::v1::Status::CODE_OK);
        EXPECT_EQ(response.get_health().devices_size(), 2);
        EXPECT_EQ(response.get_health().provider().metrics().at("inventory_mode"), "config_seeded");
    }

    {
        SCOPED_TRACE("describe_missing_device");
        anolis::deviceprovider::v1::Request bad_describe;
        bad_describe.set_request_id(6);
        bad_describe.mutable_describe_device()->set_device_id("missing0");
        const auto response = session.send(bad_describe);
        EXPECT_EQ(response.status().code(), anolis::deviceprovider::v1::Status::CODE_NOT_FOUND);
    }

    {
        SCOPED_TRACE("read_signals_unimplemented");
        anolis::deviceprovider::v1::Request read_signals;
        read_signals.set_request_id(7);
        read_signals.mutable_read_signals()->set_device_id("rlht0");
        read_signals.mutable_read_signals()->add_signal_ids("t1_c");
        const auto response = session.send(read_signals);
        EXPECT_EQ(response.status().code(), anolis::deviceprovider::v1::Status::CODE_UNAVAILABLE);
    }

    {
        SCOPED_TRACE("call_unimplemented");
        anolis::deviceprovider::v1::Request call;
        call.set_request_id(8);
        call.mutable_call()->set_device_id("dcmt0");
        call.mutable_call()->set_function_id(1);
        const auto response = session.send(call);
        EXPECT_EQ(response.status().code(), anolis::deviceprovider::v1::Status::CODE_UNAVAILABLE);
    }

    {
        SCOPED_TRACE("unsupported_hello");
        anolis::deviceprovider::v1::Request unsupported_hello;
        unsupported_hello.set_request_id(9);
        unsupported_hello.mutable_hello()->set_protocol_version("v999");
        unsupported_hello.mutable_hello()->set_client_name("shell-test");
        unsupported_hello.mutable_hello()->set_client_version("0.1.0");
        const auto response = session.send(unsupported_hello);
        EXPECT_EQ(response.status().code(), anolis::deviceprovider::v1::Status::CODE_FAILED_PRECONDITION);
    }
}

} // namespace
