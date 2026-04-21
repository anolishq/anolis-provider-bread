# Changelog

All notable changes to `anolis-provider-bread` are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Historical note: this changelog was written retrospectively from git history at the
time of the first tagged release (`v0.1.0`). Earlier development was tracked in
commit messages only.

---

## [Unreleased]

## [0.2.0] - 2026-04-21

### Changed

- Switch `anolis-protocol` dependency from git submodule to FetchContent, pinned at `v1.0.0` then bumped to `v1.1.3`.
- Cut `CRUMBS` and `bread-crumbs-contracts` dependencies to `find_package`; packages located via `CRUMBS_DIR` / `BREAD_CONTRACTS_DIR` CMake variables — removes the submodule requirement.
- Remove stale `ANOLIS_PROTOCOL_DIR` variable from `CMakePresets.json`.

### CI

- Pin org reusable workflow refs from `@main` to `@v1`.
- Add metrics collection to release workflow; `metrics.json` uploaded as release asset on each `v*` tag.

## [0.1.0] - 2026-04-20

First tagged release. The BREAD provider was developed in full before tagging; this
entry summarizes the meaningful work that landed prior to `v0.1.0`.

### Added

- Full ADPP v1 device provider implementation over gRPC: `Handshake`, `Health`,
  `ListDevices`, `DescribeDevice`, `ReadDevice`, `CallDevice`, `StreamTelemetry`.
- RLHT (Relay/Heater) hardware adapter: full session lifecycle, capability surface,
  health telemetry, and I2C-backed read/call dispatch via CRUMBS.
- DCMT (DC Motor) hardware adapter: full session lifecycle, PWM range enforcement
  (`[-255, 255]`), capability arg min/max metadata, and I2C-backed dispatch.
- `hardware.require_live_session` fail-fast guard: config-gated check that aborts
  startup when hardware session cannot be established, with supporting tests and
  troubleshooting docs.
- Session error mapping to canonical ADPP status codes.
- Health hardening: degraded state tracking, timeout tests; 87/87 tests passing
  at first release.
- Canonical `hw.bus_path` / `hw.i2c_address` descriptor tag emission on
  `DescribeDevice`, with inventory test coverage.
- C++ unit tests via GoogleTest (vcpkg) with CTest integration.
- Warnings-as-errors enforced on `ci-linux-release` preset (no-hardware lane).
- CI: Linux no-hardware lane and Windows no-hardware lane; hardware lane deferred
  pending lab CI infrastructure.
- Release workflow: on `v*` tag, builds `ci-linux-release` (no hardware), packages
  binary + source tarball + `manifest.json` + `SHA256SUMS`. Hardware lane excluded
  from automated release per `TODO.md`.

### Changed

- Migrated protocol source from `external/anolis` to `external/anolis-protocol`
  submodule.
- Wrapper scripts removed; all build/test commands use CMake/CTest presets directly.
- DCMT PWM bounds aligned with Nano hardware limits; bioreactor automation defaults
  updated accordingly.
- CI dependency checkouts pinned to immutable release tags: CRUMBS `v0.12.2`,
  `bread-crumbs-contracts` `v0.4.2`.
- Org renamed from `FEASTorg` to `anolishq` throughout.
- License changed to AGPL-3.0.

### Fixed

- GCC warnings resolved in Linux build prior to enabling warnings-as-errors.
- Hardcoded `x64-linux` triplet removed from CMake presets (was blocking
  cross-preset portability).
