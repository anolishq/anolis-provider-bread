#include "config/config_schema.hpp"

/**
 * @file config_schema.cpp
 * @brief The declare-once config schema for anolis-provider-bread.
 *
 * Titles, defaults, and placeholders carry the form-authoring knowledge the
 * workbench previously hardcoded in its provider catalog — a client renders
 * config forms from this schema alone (anolis-workbench#270).
 */

namespace anolis_provider_bread {

namespace cfg = anolis::provider_sdk::config;

namespace {

constexpr const char *kIdentifierPattern = "^[A-Za-z0-9_.-]{1,64}$";

cfg::Schema build_schema() {
    cfg::Object provider(cfg::Openness::Closed);
    provider.field(cfg::string_field("name")
                       .title("Provider name")
                       .description("Stable identifier for this provider instance.")
                       .pattern(kIdentifierPattern)
                       .default_string("anolis-provider-bread"));

    cfg::Object hardware(cfg::Openness::Closed);
    hardware.field(cfg::string_field("bus_path")
                       .required()
                       .non_empty()
                       .title("I2C bus path")
                       .placeholder("/dev/i2c-1 or mock://name"));
    hardware.field(cfg::integer_field("query_delay_us")
                       .title("Query delay (µs)")
                       .description("Delay between a CRUMBS query write and its reply read.")
                       .min_int(1)
                       .max_int(2147483647)
                       .default_int(10000));
    hardware.field(cfg::integer_field("timeout_ms")
                       .title("Reply timeout (ms)")
                       .description("How long to wait for a device's CRUMBS reply before the read fails.")
                       .min_int(1)
                       .max_int(2147483647)
                       .default_int(100));
    hardware.field(
        cfg::integer_field("retry_count").title("I/O retries").min_int(0).max_int(2147483647).default_int(2));
    hardware.field(cfg::integer_field("max_bus_hz")
                       .title("Maximum bus clock (Hz)")
                       .description("The highest I2C clock these devices tolerate on this machine. --check-host and "
                                    "startup compare it with the bus's configured clock; unset skips that check.")
                       .min_int(1)
                       .max_int(5000000));

    cfg::Object discovery(cfg::Openness::Closed);
    discovery.field(cfg::string_field("mode")
                        .required()
                        .title("Discovery mode")
                        .enum_value("scan", "Scan the bus")
                        .enum_value("manual", "Manual address list")
                        .description("manual probes exactly the listed addresses; scan walks the bus."));
    discovery.array("addresses",
                    cfg::Array::of_scalars(cfg::i2c_address_field("address"))
                        .min_items(1)
                        .unique()
                        .title("Manual addresses")
                        .description("Probed in order when mode is manual; duplicates rejected."),
                    cfg::Presence::Optional);
    // Manual discovery turns the address list into part of the contract;
    // scan must not carry one (a quietly-ignored list would mask config drift).
    discovery.when("mode", "manual", cfg::When().require("addresses"));
    discovery.when("mode", "scan", cfg::When().forbid("addresses"));

    cfg::Object device(cfg::Openness::Closed);
    device.field(cfg::string_field("id")
                     .required()
                     .unique()
                     .title("Device id")
                     .description("Stable device identifier; unique across devices.")
                     .pattern(kIdentifierPattern));
    device.field(cfg::string_field("type")
                     .required()
                     .title("Device type")
                     .enum_value("rlht", "RLHT Heater")
                     .enum_value("dcmt", "DCMT Motor"));
    device.field(cfg::string_field("label").non_empty().title("Label").description(
        "Human-readable label; defaults to the device id."));
    device.field(cfg::i2c_address_field("address")
                     .required()
                     .unique()
                     .title("I2C address")
                     .description("7-bit address in the 0x08-0x77 range; unique across devices."));
    device.field(cfg::integer_field("command_watchdog_ms")
                     .title("Command watchdog (ms)")
                     .description("Firmware command-watchdog timeout; 0 never arms. Wire field is u16.")
                     .min_int(0)
                     .max_int(65535)
                     .default_int(0));

    cfg::Object root(cfg::Openness::Closed);
    root.child("provider", std::move(provider));
    root.child("hardware", std::move(hardware), cfg::Presence::Required);
    root.child("discovery", std::move(discovery), cfg::Presence::Required);
    root.array("devices", cfg::Array::of_objects(std::move(device)));

    return cfg::Schema(std::move(root))
        .title("anolis-provider-bread configuration")
        .description("BREAD-over-CRUMBS hardware devices (RLHT, DCMT) on a shared I2C bus.");
}

}  // namespace

const cfg::Schema &provider_schema() {
    static const cfg::Schema schema = build_schema();
    return schema;
}

}  // namespace anolis_provider_bread
