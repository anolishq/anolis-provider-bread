#include "config/provider_config.hpp"
#include "logging/logger.hpp"

#include <exception>
#include <iostream>
#include <string>

#include "protocol.pb.h"

#ifdef ANOLIS_PROVIDER_BREAD_HAVE_HARDWARE
#include "bread/dcmt_ops.h"
#include "bread/rlht_ops.h"
#endif

namespace {

void print_usage(const char *program_name) {
    std::cout
        << "Usage:\n"
        << "  " << program_name << " --version\n"
        << "  " << program_name << " --check-config <path>\n\n"
        << "Phase 0 provides build, dependency, and config validation scaffolding only.\n";
}

} // namespace

int main(int argc, char **argv) {
    anolis::deviceprovider::v1::HelloResponse hello;
    hello.set_protocol_version("v1");
    hello.set_provider_name("anolis-provider-bread");
    (void)hello;

#ifdef ANOLIS_PROVIDER_BREAD_HAVE_HARDWARE
    const int known_type_ids[] = {RLHT_TYPE_ID, DCMT_TYPE_ID};
    (void)known_type_ids;
#endif

    if(argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << ANOLIS_PROVIDER_BREAD_VERSION << '\n';
        return 0;
    }

    if(argc == 3 && std::string(argv[1]) == "--check-config") {
        try {
            const anolis_provider_bread::ProviderConfig config =
                anolis_provider_bread::load_config(argv[2]);
            anolis_provider_bread::logging::info(
                "Config valid: " + anolis_provider_bread::summarize_config(config));
            anolis_provider_bread::logging::info(
                "Phase 0 foundation only; ADPP transport and hardware runtime are not implemented yet.");
            return 0;
        } catch(const std::exception &e) {
            anolis_provider_bread::logging::error(e.what());
            return 1;
        }
    }

    print_usage(argv[0]);
    return argc == 1 ? 0 : 1;
}
