# anolis-provider-bread Overview

`anolis-provider-bread` is the first real hardware provider for the Anolis runtime.

It is intentionally scoped to **BREAD-over-CRUMBS** rather than to CRUMBS in general. That scope is intentional: BREAD is the concrete hardware contract we have today, while a generic multi-family CRUMBS provider would be speculative.

## What Has Been Decided

- The provider is **BREAD-specific** in product scope.
- The implementation should still have **clean internal boundaries** so reusable CRUMBS session and bus-management code can be extracted later if a real second consumer appears.
- Generalization is allowed only where it is low-effort and structurally justified, not as an upfront abstraction exercise.
- The first implementation target is **Linux host mode** using the CRUMBS Linux HAL.

## Design Direction

The provider should stay organized around a small number of concrete responsibilities:

- **Provider core**: ADPP transport, lifecycle, configuration, logging, health, and request handling.
- **CRUMBS session layer**: Linux I2C or CRUMBS session management, scan-send-read primitives, timeout and retry behavior, bus access serialization, and diagnostics.
- **BREAD shared helpers**: small shared logic for discovery, compatibility checks, and capability fallback where that duplication is genuinely structural.
- **Device adapters**: one adapter per BREAD device type, each owning its ADPP metadata, signals, functions, and call or read translation.

A separate large family-wide abstraction layer is not assumed up front.

## Key Rules

- Keep the CRUMBS layer focused on transport, session, and bus mechanics.
- Keep BREAD version, capability, and state semantics above that layer.
- Keep CRUMBS or Linux details localized and minimal in higher layers.
- Optional behavior must be gated by BREAD capability flags, not by generation labels.
- Extraction should happen only when a second real CRUMBS-family consumer exists.

## Current Technical Basis

This repo is intended to sit on top of:

- `anolis-protocol` for ADPP contracts,
- `CRUMBS` for bus transport and Linux host support,
- `bread-crumbs-contracts` for BREAD wire contracts and compatibility rules.

Current implementation constraints include:

- `anolis-protocol` should follow the same submodule-first pattern used by the other Anolis provider repos,
- Linux-first host support via the CRUMBS Linux HAL,
- direct dependence on the current BREAD contract headers for RLHT and DCMT behavior,
- the fact that current BREAD contract headers expose CRUMBS headers and types directly,
- third-party dependency alignment with `anolis-provider-sim` through `vcpkg` using `protobuf`, `yaml-cpp`, and `gtest`,
- explicit CMake handling in `CRUMBS` so Linux HAL can consume either an installed `linux_wire::linux_wire` package target or a local build-tree bridge when `linux-wire` is added as a sibling source dependency,
- the licensing implications of consuming CRUMBS directly.

## Build Notes

See [build.md](build.md) for the current configure, build, and test flow.

## Working Notes

Committed documentation in `docs/` should stay short and stable.

Internal planning, phase breakdowns, and scratch notes belong in `working/`, which is intentionally ignored by git.
