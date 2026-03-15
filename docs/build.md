# Build Notes

Phase 1 establishes the ADPP provider shell for `anolis-provider-bread`.
Phase 2 adds the provider-owned CRUMBS session layer.
Phase 3 adds BREAD inventory and compatibility logic on top of the BREAD contract headers.
Phase 4 adds the RLHT and DCMT hardware adapters with full signal and call translation.
Phase 5 adds session error mapping, degraded-state health reporting, and timeout coverage.

## Workspace Layout

The expected local workspace is:

```text
repos_feast/
|- anolis-provider-bread/
|- CRUMBS/
|- bread-crumbs-contracts/
|- linux-wire/
```

`anolis-protocol` is expected as the repo-local submodule at `external/anolis-protocol`.

## Prerequisites

- CMake 3.20 or newer
- Ninja or Visual Studio generators
- `VCPKG_ROOT` set to a working vcpkg installation
- vcpkg manifest dependencies available for `protobuf`, `yaml-cpp`, and `gtest`
- sibling source repos for `CRUMBS` and `bread-crumbs-contracts`

## Foundation-Only Configure

Use the foundation path when you want the config-backed shell, CRUMBS session tests, and BREAD inventory logic without Linux hardware integration.

On Windows with MSVC:

```powershell
cmake --preset dev-windows-foundation-debug
cmake --build --preset dev-windows-foundation-debug
ctest --preset dev-windows-foundation-debug
```

On non-Windows hosts:

```bash
cmake --preset dev-foundation-debug
cmake --build --preset dev-foundation-debug
ctest --preset dev-foundation-debug
```

If you use MinGW on Windows, select an explicit MinGW vcpkg triplet such as `x64-mingw-dynamic` instead of the default MSVC triplet.

## Linux Hardware Configure

Use this on the real Linux host when working against CRUMBS and BREAD hardware:

```bash
cmake --preset dev-linux-debug
cmake --build --preset dev-linux-debug
ctest --preset dev-linux-debug
```

This preset expects sibling repos for `CRUMBS`, `bread-crumbs-contracts`, and `linux-wire`.
It also compiles the Linux-backed CRUMBS transport adapter used by the Phase 2 session layer.

## Running The Shell Manually

The Phase 1 provider uses framed stdio and a config-seeded inventory.

Validate config only:

```powershell
.\build\dev-windows-foundation-debug\Debug\anolis-provider-bread.exe --check-config config\example.local.yaml
```

Start the provider shell for ADPP clients:

```powershell
.\build\dev-windows-foundation-debug\Debug\anolis-provider-bread.exe --config config\example.local.yaml
```

The committed sample config seeds one RLHT and one DCMT device so `Hello`, `WaitReady`, `ListDevices`, `DescribeDevice`, and `GetHealth` are testable before real hardware work begins.

## Test Taxonomy

Two test executables are produced by every build:

| Executable | Label | Coverage | Hardware required |
|---|---|---|---|
| `provider_unit_tests` | `unit` | Config, CRUMBS session, inventory, health, RLHT/DCMT adapters | No |
| `provider_phase1_shell_tests` | `phase1` | ADPP framing and handler dispatch via provider subprocess | No |

Both executables run under the foundation presets without hardware. Hardware-backed validation
(real I2C bus, real RLHT/DCMT devices) is performed manually using the `dev-linux-*` presets
and is not automated in the foundation CI lane.

To run only unit tests locally:

```bash
ctest --preset dev-foundation-debug -L unit
```

To run all non-hardware tests:

```bash
ctest --preset dev-foundation-debug
```

## CI Lanes

Two CI presets are defined:

| Preset | Hardware | Warnings as errors | Purpose |
|---|---|---|---|
| `ci-foundation-release` | No | Yes | Automated CI on every push and PR |
| `ci-linux-release` | Yes | Yes | Manual validation on real hardware host |

The foundation lane runs automatically via GitHub Actions (`.github/workflows/ci.yml`).
It checks out `CRUMBS` and `bread-crumbs-contracts` as sibling directories and configures
with `ANOLIS_PROVIDER_BREAD_ENABLE_HARDWARE=OFF`.

The hardware lane is not automated. Run it manually on the Linux host with the real CRUMBS bus:

```bash
cmake --preset ci-linux-release
cmake --build --preset ci-linux-release
ctest --preset ci-linux-release
```

## Notes

- `CRUMBS` and `bread-crumbs-contracts` are now first-class source dependencies for all builds.
- Hardware integration is intentionally gated behind `ANOLIS_PROVIDER_BREAD_ENABLE_HARDWARE`.
- When hardware integration is enabled, the parent build should add `linux-wire` before `CRUMBS`.
- The Phase 2 session layer owns scan/send/read/query-read behavior only.
- Phase 3 inventory and compatibility code uses `bread-crumbs-contracts` for type IDs, baseline capability profiles, and compatibility rules instead of duplicating those contracts locally.
- `LinuxTransport::bind_device(...)` exists so later phases can call RLHT/DCMT helpers from `bread-crumbs-contracts` rather than rebuilding device wire logic.
- In Phase 1, `ReadSignals` and `Call` are intentionally present but return `CODE_UNIMPLEMENTED` after validating device and identifier existence.