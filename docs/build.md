# Build Notes

## Dependencies

A build needs only this repo. At configure time, CMake FetchContent downloads the source
dependencies as release tarballs, each pinned by URL and SHA256 in `CMakeLists.txt`:

- `anolis-provider-sdk`, which also carries the `anolis-protocol` pin
- `CRUMBS` (only the portable codec is built)
- `bread-crumbs-contracts`

None of them is a sibling checkout or submodule. To build against a local checkout instead,
for example while changing one of them alongside this repo, point CMake at it on configure:

| Dependency | Configure option |
|---|---|
| `CRUMBS` | `-DCRUMBS_DIR=<path>` |
| `bread-crumbs-contracts` | `-DBREAD_CONTRACTS_DIR=<path>` |
| `anolis-provider-sdk` | `-DFETCHCONTENT_SOURCE_DIR_ANOLIS_PROVIDER_SDK=<path>` |

A relative `CRUMBS_DIR` or `BREAD_CONTRACTS_DIR` resolves against this repo's root, so
`-DCRUMBS_DIR=../CRUMBS` picks up a sibling clone.

## Prerequisites

- CMake 3.24 or newer for the Ninja presets (Linux, macOS); CMake 4.2 or newer for the
  Windows presets, whose `Visual Studio 18 2026` generator was added in CMake 4.2
- Ninja or Visual Studio generators
- `VCPKG_ROOT` set to a working vcpkg installation
- vcpkg manifest dependencies available for `protobuf`, `yaml-cpp`, and `gtest`
- network access at configure time, to fetch the pinned source tarballs

### Install Dependencies (Linux)

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build git curl zip unzip tar pkg-config python3 python3-pip
```

Install vcpkg:

```bash
git clone https://github.com/microsoft/vcpkg.git "$HOME/vcpkg"
"$HOME/vcpkg/bootstrap-vcpkg.sh"
echo 'export VCPKG_ROOT="$HOME/vcpkg"' >> ~/.bashrc
export VCPKG_ROOT="$HOME/vcpkg"
test -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
```

### Install Dependencies (Windows)

```powershell
winget install Kitware.CMake
winget install Ninja-build.Ninja
winget install Git.Git
winget install Python.Python.3.12
```

Install Visual Studio 2026 (or Build Tools for Visual Studio 2026) with the
`Desktop development with C++` workload. The Windows presets use the `Visual Studio 18 2026`
generator, so they do not configure with an older Visual Studio.

Install vcpkg:

```powershell
git clone https://github.com/microsoft/vcpkg.git $env:USERPROFILE\vcpkg
& "$env:USERPROFILE\vcpkg\bootstrap-vcpkg.bat"
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", "$env:USERPROFILE\vcpkg", "User")
$env:VCPKG_ROOT = [Environment]::GetEnvironmentVariable("VCPKG_ROOT", "User")
Test-Path "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
```

## Configure And Build

On Linux, one binary covers both hardware and no-hardware operation. Hardware
support is always compiled into the Linux binary; whether the provider touches
real hardware is decided at runtime by `hardware.bus_path` in the config (see
[Running The Provider Manually](#running-the-provider-manually)), not by a build
flag. Windows and macOS builds are config-seeded only, whatever `hardware.bus_path`
says. The presets below build and test without real hardware on all platforms,
covering config loading, CRUMBS session logic, BREAD inventory, and ADPP handler
dispatch.

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

## Running Against Real Hardware

There is no separate hardware preset or build flag. The `dev-debug` / `dev-release`
builds above already include hardware capability on Linux (the SDK's Linux I2C bus
is always compiled in).
To work against real CRUMBS and BREAD hardware, run that same binary with a config
whose `hardware.bus_path` points at a real device node (e.g. `/dev/i2c-1`); any
non-`mock://` path makes the provider check host requirements and then open a live
CRUMBS session on that bus.

On ARM hosts (for example Raspberry Pi), the default preset uses vcpkg's host-default
triplet. If you need an explicit override, pass `-DVCPKG_TARGET_TRIPLET=<triplet>`
(and optionally `-DVCPKG_HOST_TRIPLET=<triplet>`) on configure.

## Running The Provider Manually

Validate config only:

```powershell
.\build\dev-windows-debug\Debug\anolis-provider-bread.exe --check-config config\example.local.yaml
```

Start the provider for ADPP clients:

```powershell
.\build\dev-windows-debug\Debug\anolis-provider-bread.exe --config config\example.local.yaml
```

The committed sample config declares one RLHT and one DCMT device on `/dev/i2c-1`. On Windows
and macOS it seeds them without bus access, so `Hello`, `WaitReady`, `ListDevices`,
`DescribeDevice`, and `GetHealth` are testable without real hardware.

To exercise the config-seeded inventory without hardware, set `hardware.bus_path` to a
`mock://...` value (as in `config/ci.test.yaml`). On Linux, a real bus path that cannot be opened,
or whose host requirements are unmet, does not fall back to a seeded inventory: the provider
keeps serving ADPP but reports not ready, with every configured device listed as missing and
the reason given.

## Test Taxonomy

Two test executables are produced by every build:

| Executable | Label | Coverage | Hardware required |
|---|---|---|---|
| `provider_unit_tests` | `unit` | Config, CRUMBS session, inventory, health, RLHT/DCMT adapters | No |
| `provider_shell_tests` | `shell` | ADPP framing and handler dispatch via provider subprocess | No |

Both executables run without hardware. Hardware-backed validation (real I2C bus, real RLHT/DCMT
devices) is performed manually by running the provider with a real `hardware.bus_path`, and is
not automated in CI.

To run only unit tests locally:

```bash
ctest --preset dev-debug -L unit
```

To run all non-hardware tests:

```bash
ctest --preset dev-debug
```

## CI Lanes

| Preset | Warnings as errors | Purpose |
|---|---|---|
| `ci-linux-release` | Yes | CI on pushes to `main`, PRs against `main` and manual dispatch; skipped when only `docs/`, Markdown, PNG or SVG files change (Linux x64) |
| `ci-linux-arm64-release` | Yes | Same triggers as `ci-linux-release`, run natively on `ubuntu-24.04-arm` (Linux arm64) |
| `ci-windows-release` | Yes | Same triggers as `ci-linux-release` (Windows) |

The `ci-linux-release`, `ci-linux-arm64-release` and `ci-windows-release` lanes run automatically via GitHub Actions
(`.github/workflows/ci.yml`). They check out only this repo, fetch the pinned dependencies at
configure time, and exercise the provider against a `mock://` bus (no I2C hardware).

There is no automated hardware lane. Real-hardware validation is performed manually by running
the released binary on the Linux host with a real `hardware.bus_path` pointing at the CRUMBS bus.

## Notes

- `CRUMBS` and `bread-crumbs-contracts` are source dependencies for all builds, fetched as described in [Dependencies](#dependencies).
- On Linux, hardware vs config-seeded operation is selected at runtime by `hardware.bus_path` (`mock://...` vs a real path), not by a build flag. Windows and macOS builds are always config-seeded.
- Only the portable CRUMBS codec is built (`CRUMBS_ENABLE_LINUX_HAL` is forced `OFF`); raw I2C goes through the SDK's Linux I2C bus.
- The CRUMBS session layer (`src/crumbs/`) owns bus and session behavior (open, scan, send, read, query-read, retries, error
  normalization, and per-address I/O stats, recovery and rejection tracking), plus the CRUMBS
  transport and the canned mock bus. The session logic carries no RLHT/DCMT semantics; the canned
  mock bus holds fixed RLHT/DCMT state replies for mock mode.
- Inventory and compatibility code (`src/devices/common/`) uses `bread-crumbs-contracts` for type
  IDs, baseline capability profiles, and compatibility rules.
