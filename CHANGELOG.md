# Changelog

All notable changes to `anolis-provider-bread` are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Historical note: this changelog was written retrospectively from git history at the
time of the first tagged release (`v0.1.0`). Earlier development was tracked in
commit messages only.

---

## [Unreleased]

### Fixed

- RLHT sentinel temperatures no longer surface as fabricated readings (#109).
  The firmware sends `BREAD_INVALID_I16` for an open thermocouple, and the
  adapter divided the sentinel into **-3276.8 °C with quality OK** on
  `t1_c`/`t2_c` (and sentinel setpoints alike). Sentinel-eligible deci-C
  signals now emit `QUALITY_FAULT` with a placeholder `0.0`, so consumers
  keyed on the quality field never act on a fabricated temperature.

## [0.3.3] - 2026-07-14

### Fixed

- Startup probes re-issue the SET_REPLY round-trip (up to 3 times) when a
  device serves a reply for a different opcode (#103). A stale staged reply
  is an expected transient after an ungracefully interrupted master — its
  dying poll leaves `requested_opcode` staged on the peripheral — and
  previously excluded the device from inventory until the next restart.
  Observed twice on the #138 bench with a different victim each restart.

### Added

- Real per-device health (#87): `DeviceHealth.last_seen` is now the wall-clock
  time of the last successful CRUMBS operation against the device's address
  (probes included), and three new per-device metrics expose the session's
  cumulative I/O counters — `io_ok`, `io_failed`, and `io_retried_attempts`.
  The retry counter records every attempt beyond an operation's first, so
  intermittent bus trouble that the retry policy masks stays visible in health
  output instead of silently disappearing (the mechanism that hid a ~30%
  per-transaction failure rate for weeks, see feastorg/Slice_DCMT#3).
- Missing-expected devices whose configured address was probed and failed now
  carry the probe failure into health output (#104): the readiness
  `failed_devices` reason and a `missing_detail` health metric report WHY the
  device is absent (e.g. `probe failed (version_read_failed): unexpected
  opcode in version reply: 0x80`) instead of the generic "not found during
  startup".

### Changed

- `last_seen` is no longer fabricated: devices with no successful contact yet
  (including missing-expected devices) leave it unset, replacing the previous
  provider-start-time constant stamped on every device.

## [0.3.2] - 2026-07-14

### Changed

- Bumped dependency pins: CRUMBS `0.12.4` → `0.12.5` (upstream fix for
  feastorg/CRUMBS#15 — host read paths now trim padded reads before the
  exact-length decode) and bread-crumbs-contracts `0.4.3` → `0.4.4`
  (new `dcmt_parse_state_payload` / `rlht_parse_state_payload`).
- `LinuxTransport::read` now trims padded reads with CRUMBS'
  `crumbs_frame_length()`; the local `crumbs_reply_frame_length` workaround
  from 0.3.1 and its hand-rolled frame geometry are deleted.
- DCMT and RLHT adapters parse GET_STATE payloads through the contracts'
  new parsers instead of hand-rolled per-field extraction; wire layout
  knowledge now lives only in bread-crumbs-contracts. The RLHT parser
  requires the full 19-byte payload (previously bytes 18+ were ignored),
  matching what the firmware has always sent.
- Frame-header validation failures on the read path (garbage or truncated
  headers, e.g. an all-`0xFF` read from a silent device) are now uniformly
  classified `read_failed`; previously an out-of-range declared payload
  length surfaced as `decode_failed`. CRC mismatches remain `decode_failed`.

### Removed

- Removed the unused `LinuxTransport::bind_device()` bridge. The contracts'
  `_get_*` round-trip helpers are deliberately not used by this provider —
  they hardcode the query delay and carry no locking or retry, which
  `Session` provides.

## [0.3.1] - 2026-07-13

### Fixed

- **Regression since 0.2.9 — live CRUMBS reads stopped decoding on Linux.** Every
  hardware read failed as `code=read_failed native_code=-1 "failed to decode
  CRUMBS reply"`, no device entered the inventory, and the runtime aborted with
  `No devices could be registered from provider bread0`.

  `LinuxTransport::read` has always handed `crumbs_decode_message()` the *raw read
  length*. On Linux that is the buffer size, not the frame size: an I2C controller
  must request a fixed byte count up front, a CRUMBS peripheral writes only its
  actual frame (`Wire.write(frame, frame_len)`), and the bus then floats high — so
  the tail of the read is `0xFF` padding.

  CRUMBS `v0.12.2` tolerated that: its decoder ignored trailing bytes and decoded
  the prefix, so live reads worked. **CRUMBS `v0.12.4` (2026-06-10) tightened
  `crumbs_decode_message()` to enforce the documented exact-frame-length contract
  — trailing bytes now return `-1`.** bread `0.2.9` (2026-06-11) bumped the CRUMBS
  pin `v0.12.2 → v0.12.4`, and live reads have failed ever since. The transport was
  always violating the contract; 0.12.4 began enforcing it.

  The read is now trimmed to the frame length its header declares before decoding.
  The CRC is still validated, over the trimmed span. The computation is extracted
  as `crumbs_reply_frame_length()` so it is unit-testable without a live bus.

  CI did not catch the regression because every automated surface uses `mock://`,
  which returns exact frames with no bus padding — only real Linux i2c-dev
  reproduces it. Caught on a Raspberry Pi against live Slice_RLHT / Slice_DCMT
  boards (anolishq/anolis#138).

## [0.3.0] - 2026-07-04

### Added

- **Migrated onto `anolis-provider-sdk`.** The provider now builds on the shared
  SDK's `ProviderRuntime` + run-loop and device-model framework rather than a
  private copy, pinned to SDK v0.1.2. (#86, #88, #89)
- **Per-device health metrics + `last_seen`** — health enrichment surfaced per
  device (SDK#9). (#88)
- **Mock bus backend** — live mock-mode reads and calls without hardware, for
  bench/CI. (#81)
- Genuine `init_time_ms` reported in `WaitReady`; the prior waiver is dropped.
  (#78)
- Curated default signal set (#54); `CallResponse.results` populated (#53);
  `min_timestamp` honored best-effort on reads (#56).

### Changed

- Device-model refactors toward the SDK: `DeviceType` moved into the devices
  layer; `DeviceAdapter` descriptor + guarded `adapter_for` switch; C++20
  `std::format` adopted for diagnostics.

### Fixed

- `Device.provider_name` now matches the value declared in Hello. (#55)

### CI

- Native arm64 unit-test lane; TSAN + Valgrind parity with ezo; clang-tidy diff
  gate promoted to blocking; pinned clang-format / clang-tools; routine
  dependency maintenance.

## [0.2.11] - 2026-06-22

### Added

- **ADPP conformance level 2.** Declare `conformance_level = 2` in
  `config/conformance.toml` (one waiver retained: `init_time_ms`, tracked in
  #45). The provider satisfies the L2 clauses of the ADPP semantics:
  - Reject a non-Hello request received before a successful Hello with
    `CODE_FAILED_PRECONDITION` (§3.2).
  - Enforce declared numeric bounds as `CODE_OUT_OF_RANGE` and reject non-finite
    doubles (`NaN`/`±Inf`) with `CODE_INVALID_ARGUMENT` (§8.3).

### Changed

- Validate and encode a `Call`'s arguments before touching the hardware session,
  so argument errors surface as `INVALID_ARGUMENT` / `OUT_OF_RANGE` regardless of
  whether a hardware session is available (§8.3). Each adapter's `call()` is split
  into `build_frame()` + `transmit()`.

### CI

- Add the ADPP `provider.conformance` lane: run the pinned
  `anolis-adpp-conformance` harness against the built binary using the
  provider-owned `config/conformance.toml` manifest.
- Add a keyless dependency/CVE scan (`cve-bin-tool`) lane.
- Routine dependency maintenance: refresh pinned GitHub Actions to the current
  org-tracked revisions.

## [0.2.10] - 2026-06-16

### Changed

- Bump the vcpkg baseline to the vcpkg `2026.06.01` release: protobuf
  `5.29.5` → `6.33.4`, grpc `1.71.0` → `1.76.0`, abseil and the rest refreshed.
  No source changes required.

### CI

- Fix Windows build on the VS 2026 runner image. The hosted `windows-2025`
  image moved from Visual Studio 2022 to VS 2026, breaking the hardcoded
  `Visual Studio 17 2022` generator at CMake `project()`. Update the
  `base-windows-msvc` preset generator to `Visual Studio 18 2026`. The plain
  `x64-windows` triplet inherits the image's default toolset (`v145`), so no
  triplet/toolset change is required.
- Centralize the vcpkg pin: the shared `setup-vcpkg` action now derives the
  vcpkg commit from `vcpkg-configuration.json`, so the per-workflow
  `VCPKG_COMMIT` env was removed.

## [0.2.9] - 2026-06-11

### Fixed

- Decode DCMT telemetry from the fixed-layout `bread-crumbs-contracts` state
  payload and map mode-specific fields onto the stable ADPP motor target/value
  signals.

### Changed

- Bump provider metadata to `0.2.9`.
- Bump `CRUMBS` FetchContent pin from `v0.12.2` to `v0.12.4`.
- Bump `bread-crumbs-contracts` FetchContent pin from `v0.4.2` to `v0.4.3`.

### CI

- Add CI OK aggregator gate: removed `paths-ignore`, added `dorny/paths-filter`
  to detect code-vs-docs changes, gated all jobs behind the filter, and added a
  final `ok` job as the sole required status check for `main` branch protection.

## [0.2.7] - 2026-04-24

### Changed

- Updated `anolis-protocol` dependency from v1.1.4 to v1.2.0. The new release
  adds `optional` presence to `ArgSpec` bounds fields, enabling zero-valued
  bounds and fixing one-sided bound enforcement in the runtime. No source
  changes required in this provider — all existing `set_min_*()` / `set_max_*()`
  calls are unaffected.

## [0.2.6] - 2026-04-24

### Build

- Added `triplets/x64-linux-static.cmake` and wired it into the
  `ci-linux-release` preset (`VCPKG_TARGET_TRIPLET=x64-linux-static`,
  `VCPKG_OVERLAY_TRIPLETS`). The released Linux binary now statically links
  all vcpkg dependencies (protobuf, yaml-cpp, openssl) and is self-contained
  on any glibc Linux system without requiring vcpkg to be installed.

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
