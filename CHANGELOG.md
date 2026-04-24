# Changelog

All notable changes to `anolis-provider-bread` are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Historical note: this changelog was written retrospectively from git history at the
time of the first tagged release (`v0.1.0`). Earlier development was tracked in
commit messages only.

---

## [Unreleased]

## [0.2.5] - 2026-04-24

### Fixed

- Remove unused `parse_bool_value()` from `provider_config.cpp` (was
  triggering `-Werror=unused-function` on Linux after `require_live_session`
  was removed in v0.2.4).
- `shell_test.cpp`: update `provider_name` expectation to `"bread-ci-test"`
  to match `config/ci.test.yaml`.
- CI (`ci.yml`): align `no-hardware-linux` job with release workflow — check
  out CRUMBS, bread-crumbs-contracts, and linux-wire as sibling repos instead
  of installing from tarballs; removes explicit `-DCMAKE_PREFIX_PATH` override.

## [0.2.4] - 2026-04-24

### Changed

- Replaced compile-time `ANOLIS_PROVIDER_BREAD_ENABLE_HARDWARE` flag with runtime
  gating via `bus_path` prefix. `bus_path: mock://...` produces a config-seeded
  inventory without requiring any hardware; any other path opens a live CRUMBS
  session. This aligns bread with the ezo provider pattern: one binary per
  platform, behavior determined by config at runtime.
- Removed `hardware.require_live_session` config field. The same intent is now
  expressed by using a real bus path (e.g. `/dev/i2c-1`) — if the bus cannot be
  opened, startup throws. Configs with `require_live_session` will be rejected
  at load time as an unknown key.
- Added `config/ci.test.yaml` with `bus_path: mock://ci-test` for CI and dev
  testing without hardware.
- `linux-wire` is now always compiled into the provider on Linux (no flag required).
  `set(BUILD_SHARED_LIBS OFF)` added before the linux-wire subdirectory to ensure
  a static archive regardless of the parent CMake context.
- Removed `dev-linux-hardware-debug`, `dev-linux-hardware-release`, and
  `ci-linux-hardware-release` presets (superseded by `dev-debug`, `dev-release`,
  and `ci-linux-release` which now always include hardware capability on Linux).
- Renamed hidden `linux-hardware-base` preset to `linux-base`.

### CI

- Release workflow: added linux-wire checkout (`feastorg/linux-wire@v0.1.2`).
- Release workflow: removed explicit `-DCRUMBS_DIR` / `-DBREAD_CONTRACTS_DIR`
  cmake overrides; these are now set by the `linux-base` preset via
  `${sourceDir}/../CRUMBS` and `${sourceDir}/../bread-crumbs-contracts`.

## [0.2.3] - 2026-04-23

### CI

- Fixed release workflow: pass `CRUMBS_DIR` and `BREAD_CONTRACTS_DIR` to cmake
  configure so checked-out sources are used as `add_subdirectory` trees instead
  of requiring an installed package (resolves `find_package(crumbs)` failure).

## [0.2.2] - 2026-04-23

### Changed

- Bump `anolis-protocol` FetchContent pin from `v1.1.3` to `v1.1.4`.
- Reverted `ci-linux-release` preset: `CRUMBS_DIR` / `BREAD_CONTRACTS_DIR` variables
  removed; build now uses installed packages via `find_package`.

### CI

- Version-sync check wired: `version-locations.txt` added tracking `CMakeLists.txt`
  and `vcpkg.json`; CI calls reusable `version-sync` workflow from `anolishq/.github`.
- `vcpkg.json` version aligned to `0.2.0` (was stale at `0.1.0`).
- `.anpkg` added to `.gitignore`.

### Docs

- Build setup guide updated: sibling-checkout pattern replaced with artifact-first
  description.

## [0.2.1] - 2026-04-21

### CI

- Fix `ci-linux-release` preset to pass `CRUMBS_DIR` and `BREAD_CONTRACTS_DIR`
  to the CMake configure step.

> **Note:** the `v0.2.1` tag was applied to a CI-only commit; version strings in
> source remained at `0.2.0`. This entry is recorded for completeness.

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
