#include <exception>
#include <iostream>
#include <string>

#include "anolis/provider_sdk/config.hpp"
#include "anolis/provider_sdk/host_check.hpp"
#include "anolis/provider_sdk/runtime.hpp"
#include "config/config_schema.hpp"
#include "config/provider_config.hpp"
#include "core/bread_provider_runtime.hpp"
#include "core/host_check.hpp"
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
              << "  " << program_name << " --config-schema\n"
              << "  " << program_name << " --check-config <path>\n"
              << "  " << program_name << " --check-host <path>\n"
              << "  " << program_name << " --config <path>\n\n"
              << "Implements ADPP v1 over BREAD-over-CRUMBS with config-seeded "
                 "or hardware-backed inventory.\n";
}

}  // namespace

int main(int argc, char **argv) {
    anolis_provider_bread::runtime::reset();

    std::string config_path;
    bool check_config_only = false;
    bool check_host_only = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--version") {
            std::cout << ANOLIS_PROVIDER_BREAD_VERSION << '\n';
            return 0;
        }
        if (arg == "--config-schema") {
            // Configless (executable profile v1 §2): the envelope is the only
            // stdout output; diagnostics go to stderr. Handled before any
            // other work can touch stdout.
            anolis::provider_sdk::config::EnvelopeOptions options;
            options.provider_name = "anolis-provider-bread";
            options.extra_string_entries.emplace_back("provider_version", ANOLIS_PROVIDER_BREAD_VERSION);
            anolis::provider_sdk::config::write_config_schema_envelope(
                std::cout, anolis_provider_bread::provider_schema(), options);
            return 0;
        }
        if (arg == "--check-config" && i + 1 < argc) {
            config_path = argv[++i];
            check_config_only = true;
            continue;
        }
        if (arg == "--check-host" && i + 1 < argc) {
            config_path = argv[++i];
            check_host_only = true;
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

    if (check_host_only) {
        // Executable profile v1 §6: the envelope is the only stdout output; exit
        // 0 nothing unmet, 1 something unmet, 2 could not evaluate (bad config).
        anolis_provider_bread::ProviderConfig config;
        try {
            config = anolis_provider_bread::load_config(config_path);
        } catch (const std::exception &e) {
            anolis_provider_bread::logging::error(e.what());
            return 2;
        }
        return anolis::provider_sdk::host_check::write_envelope(std::cout, "anolis-provider-bread",
                                                                anolis_provider_bread::check_host(config));
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
    // Not ready (unmet host requirements or a failed bus open) still serves ADPP,
    // so the runtime can see why; say which it is.
    anolis_provider_bread::logging::info(anolis_provider_bread::runtime::snapshot().ready
                                             ? "ready (transport=stdio+uint32_le)"
                                             : "serving, not ready (transport=stdio+uint32_le)");

    // The SDK run-loop owns the transport, the §3.2 Hello-gate, dispatch, and exit
    // codes; bread supplies the device/inventory/readiness seam. No lifecycle hooks
    // (no physics ticker; the CRUMBS session is torn down at process exit).
    anolis_provider_bread::BreadProviderRuntime bread_runtime;
    return anolis::provider_sdk::run_loop(std::cin, std::cout, bread_runtime);
}
