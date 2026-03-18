# Build Notes

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

## No-Hardware Configure

Use this when you want to build and test without real hardware. Works on all platforms,
covers config loading, CRUMBS session logic, BREAD inventory, and ADPP handler dispatch.

On Windows with MSVC:

```powershell
cmake --preset dev-windows-debug
cmake --build --preset dev-windows-debug
ctest --preset dev-windows-debug
```

On Linux or macOS:

```bash
cmake --preset dev-debug
cmake --build --preset dev-debug
ctest --preset dev-debug
```

If you use MinGW on Windows, select an explicit MinGW vcpkg triplet such as `x64-mingw-dynamic`
instead of the default MSVC triplet.

## Linux Hardware Configure

Use this on the real Linux host when working against CRUMBS and BREAD hardware:

```bash
cmake --preset dev-linux-hardware-debug
cmake --build --preset dev-linux-hardware-debug
ctest --preset dev-linux-hardware-debug
```

This preset enables `ANOLIS_PROVIDER_BREAD_ENABLE_HARDWARE=ON` and expects sibling repos for
`CRUMBS`, `bread-crumbs-contracts`, and `linux-wire`.

## Running The Provider Manually

Validate config only:

```powershell
.\build\dev-windows-debug\Debug\anolis-provider-bread.exe --check-config config\example.local.yaml
```

Start the provider for ADPP clients:

```powershell
.\build\dev-windows-debug\Debug\anolis-provider-bread.exe --config config\example.local.yaml
```

The committed sample config seeds one RLHT and one DCMT device so `Hello`, `WaitReady`,
`ListDevices`, `DescribeDevice`, and `GetHealth` are testable without real hardware.

## Test Taxonomy

Two test executables are produced by every build:

| Executable | Label | Coverage | Hardware required |
|---|---|---|---|
| `provider_unit_tests` | `unit` | Config, CRUMBS session, inventory, health, RLHT/DCMT adapters | No |
| `provider_shell_tests` | `shell` | ADPP framing and handler dispatch via provider subprocess | No |

Both executables run without hardware. Hardware-backed validation (real I2C bus, real RLHT/DCMT
devices) is performed manually using the `dev-linux-hardware-*` presets and is not automated in CI.

To run only unit tests locally:

```bash
ctest --preset dev-debug -L unit
```

To run all non-hardware tests:

```bash
ctest --preset dev-debug
```

## CI Lanes

| Preset | Hardware | Warnings as errors | Purpose |
|---|---|---|---|
| `ci-linux-release` | No | Yes | Automated CI on every push and PR (Linux) |
| `ci-windows-release` | No | Yes | Automated CI on every push and PR (Windows) |
| `ci-linux-hardware-release` | Yes | Yes | Manual validation on real hardware host |

The `ci-linux-release` and `ci-windows-release` lanes run automatically via GitHub Actions
(`.github/workflows/ci.yml`). They check out `CRUMBS` and `bread-crumbs-contracts` as sibling
directories and configure with `ANOLIS_PROVIDER_BREAD_ENABLE_HARDWARE=OFF`.

The hardware lane is not automated. Run it manually on the Linux host with the real CRUMBS bus:

```bash
cmake --preset ci-linux-hardware-release
cmake --build --preset ci-linux-hardware-release
ctest --preset ci-linux-hardware-release
```

## Notes

- `CRUMBS` and `bread-crumbs-contracts` are first-class source dependencies for all builds.
- Hardware integration is gated behind `ANOLIS_PROVIDER_BREAD_ENABLE_HARDWARE`.
- When hardware integration is enabled, the parent build should add `linux-wire` before `CRUMBS`.
- The CRUMBS session layer (`src/crumbs/`) owns scan/send/read/query-read behavior only.
- Inventory and compatibility code (`src/devices/common/`) uses `bread-crumbs-contracts` for type
  IDs, baseline capability profiles, and compatibility rules.
