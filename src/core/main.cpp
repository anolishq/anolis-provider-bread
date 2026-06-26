#include <exception>
#include <iostream>
#include <string>

#include "anolis/provider_sdk/runtime.hpp"
#include "config/provider_config.hpp"
#include "core/bread_provider_runtime.hpp"
#include "core/runtime_state.hpp"
#include "logging/logger.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

void set_binary_mode_stdio() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

void print_usage(const char *program_name) {
    std::cout << "Usage:\n"
              << "  " << program_name << " --version\n"
              << "  " << program_name << " --check-config <path>\n"
              << "  " << program_name << " --config <path>\n\n"
              << "Implements ADPP v1 over BREAD-over-CRUMBS with config-seeded "
                 "or hardware-backed inventory.\n";
}

}  // namespace

int main(int argc, char **argv) {
    anolis_provider_bread::runtime::reset();

    std::string config_path;
    bool check_config_only = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--version") {
            std::cout << ANOLIS_PROVIDER_BREAD_VERSION << '\n';
            return 0;
        }
        if (arg == "--check-config" && i + 1 < argc) {
            config_path = argv[++i];
            check_config_only = true;
            continue;
        }
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
            continue;
        }
        print_usage(argv[0]);
        return 1;
    }

    if (config_path.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    try {
        const anolis_provider_bread::ProviderConfig config = anolis_provider_bread::load_config(config_path);

        if (check_config_only) {
            anolis_provider_bread::logging::info("Config valid: " + anolis_provider_bread::summarize_config(config));
            return 0;
        }

        anolis_provider_bread::logging::info("starting with config: " +
                                             anolis_provider_bread::summarize_config(config));
        anolis_provider_bread::runtime::initialize(config);
    } catch (const std::exception &e) {
        anolis_provider_bread::logging::error(e.what());
        return 1;
    }

    set_binary_mode_stdio();
    anolis_provider_bread::logging::info("ready (transport=stdio+uint32_le)");

    // The SDK run-loop owns the transport, the §3.2 Hello-gate, dispatch, and exit
    // codes; bread supplies the device/inventory/readiness seam. No lifecycle hooks
    // (no physics ticker; the CRUMBS session is torn down at process exit).
    anolis_provider_bread::BreadProviderRuntime bread_runtime;
    return anolis::provider_sdk::run_loop(std::cin, std::cout, bread_runtime);
}
