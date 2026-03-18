#include "config/provider_config.hpp"
#include "core/handlers.hpp"
#include "core/runtime_state.hpp"
#include "core/transport/framed_stdio.hpp"
#include "logging/logger.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "protocol.pb.h"

namespace {

void set_binary_mode_stdio() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

void print_usage(const char *program_name) {
    std::cout
        << "Usage:\n"
        << "  " << program_name << " --version\n"
        << "  " << program_name << " --check-config <path>\n"
        << "  " << program_name << " --config <path>\n\n"
        << "Implements ADPP v1 over BREAD-over-CRUMBS with config-seeded or hardware-backed inventory.\n";
}

} // namespace

int main(int argc, char **argv) {
    anolis_provider_bread::runtime::reset();

    std::string config_path;
    bool check_config_only = false;

    for(int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if(arg == "--version") {
            std::cout << ANOLIS_PROVIDER_BREAD_VERSION << '\n';
            return 0;
        }
        if(arg == "--check-config" && i + 1 < argc) {
            config_path = argv[++i];
            check_config_only = true;
            continue;
        }
        if(arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
            continue;
        }
        print_usage(argv[0]);
        return 1;
    }

    if(config_path.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    try {
        const anolis_provider_bread::ProviderConfig config =
            anolis_provider_bread::load_config(config_path);

        if(check_config_only) {
            anolis_provider_bread::logging::info(
                "Config valid: " + anolis_provider_bread::summarize_config(config));
            return 0;
        }

        anolis_provider_bread::logging::info(
            "starting provider shell with config: " +
            anolis_provider_bread::summarize_config(config));
        anolis_provider_bread::runtime::initialize(config);
    } catch(const std::exception &e) {
        anolis_provider_bread::logging::error(e.what());
        return 1;
    }

    set_binary_mode_stdio();
    anolis_provider_bread::logging::info("ready (transport=stdio+uint32_le)");

    std::vector<uint8_t> frame;
    std::string io_error;

    while(true) {
        frame.clear();
        const bool read_ok = anolis_provider_bread::transport::read_frame(std::cin, frame, io_error);
        if(!read_ok) {
            if(io_error.empty()) {
                anolis_provider_bread::logging::info("EOF on stdin; exiting cleanly");
                return 0;
            }
            anolis_provider_bread::logging::error("read_frame error: " + io_error);
            return 2;
        }

        anolis::deviceprovider::v1::Request request;
        if(!request.ParseFromArray(frame.data(), static_cast<int>(frame.size()))) {
            anolis_provider_bread::logging::error("failed to parse Request protobuf");
            return 3;
        }

        anolis::deviceprovider::v1::Response response;
        response.set_request_id(request.request_id());
        response.mutable_status()->set_code(anolis::deviceprovider::v1::Status::CODE_INTERNAL);
        response.mutable_status()->set_message("uninitialized");

        if(request.has_hello()) {
            anolis_provider_bread::handlers::handle_hello(request.hello(), response);
        } else if(request.has_wait_ready()) {
            anolis_provider_bread::handlers::handle_wait_ready(request.wait_ready(), response);
        } else if(request.has_list_devices()) {
            anolis_provider_bread::handlers::handle_list_devices(request.list_devices(), response);
        } else if(request.has_describe_device()) {
            anolis_provider_bread::handlers::handle_describe_device(request.describe_device(), response);
        } else if(request.has_read_signals()) {
            anolis_provider_bread::handlers::handle_read_signals(request.read_signals(), response);
        } else if(request.has_call()) {
            anolis_provider_bread::handlers::handle_call(request.call(), response);
        } else if(request.has_get_health()) {
            anolis_provider_bread::handlers::handle_get_health(request.get_health(), response);
        } else {
            anolis_provider_bread::handlers::handle_unimplemented(response, "operation not implemented");
        }

        std::string payload;
        if(!response.SerializeToString(&payload)) {
            anolis_provider_bread::logging::error("failed to serialize Response protobuf");
            return 4;
        }

        if(!anolis_provider_bread::transport::write_frame(
               std::cout,
               reinterpret_cast<const uint8_t *>(payload.data()),
               payload.size(),
               io_error)) {
            anolis_provider_bread::logging::error("write_frame error: " + io_error);
            return 5;
        }
    }
}
