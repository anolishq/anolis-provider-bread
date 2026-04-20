# anolis-provider-bread

`anolis-provider-bread` is the BREAD hardware provider for the Anolis runtime.

It is scoped to **BREAD-over-CRUMBS** rather than to CRUMBS in general. BREAD is the concrete hardware contract; a generic multi-family CRUMBS provider would be speculative and is not the goal.

## Scope

- The provider is **BREAD-specific**: it speaks BREAD-over-CRUMBS, not CRUMBS in general.
- Internal boundaries are organized so the CRUMBS session and bus-management code can be extracted if a second concrete consumer appears.
- Hardware support targets **Linux host mode** via the CRUMBS Linux HAL.

## Design Direction

The provider should stay organized around a small number of concrete responsibilities:

- **Provider core**: ADPP transport, lifecycle, configuration, logging, health, and request handling.
- **CRUMBS session layer**: Linux I2C or CRUMBS session management, scan-send-read primitives, timeout and retry behavior, bus access serialization, and diagnostics.
- **BREAD shared helpers**: small shared logic for discovery, compatibility checks, and capability fallback where that duplication is genuinely structural.
- **Device adapters**: one adapter per BREAD device type, each owning its ADPP metadata, signals, functions, and call or read translation.

A family-wide abstraction layer is not included; it would be speculative given the current single hardware family.

## Key Rules

- Keep the CRUMBS layer focused on transport, session, and bus mechanics.
- Keep BREAD version, capability, and state semantics above that layer.
- Keep CRUMBS or Linux details localized and minimal in higher layers.
- Optional behavior must be gated by BREAD capability flags, not by generation labels.
- Extraction should happen only when a second real CRUMBS-family consumer exists.

## Dependencies

This provider depends on:

- `anolis-protocol` — ADPP proto contracts, consumed as the repo-local submodule at `external/anolis-protocol`
- `CRUMBS` — bus transport and Linux HAL, consumed as a sibling source dependency
- `bread-crumbs-contracts` — BREAD wire contracts, type IDs, and compatibility rules, consumed as a sibling source dependency

Third-party dependencies (`protobuf`, `yaml-cpp`, `gtest`) are managed through vcpkg in alignment with `anolis-provider-sim`.

Current BREAD contract headers expose CRUMBS headers and types directly; code above the CRUMBS session layer depends on this header transitivity.

## Build Notes

See [build.md](build.md) for the current configure, build, and test flow.

## Troubleshooting

See [troubleshooting.md](troubleshooting.md) for common failure modes, log signatures, and fixes.

## Versioning

See [versioning.md](versioning.md) for provider and dependency version expectations.

## Local API docs

Run `doxygen docs/Doxyfile` from the repo root.
Generated HTML goes to `build/docs/doxygen/html/` and should not be committed.
