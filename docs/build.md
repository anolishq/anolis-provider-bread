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

`anolis-protocol` is fetched automatically via CMake FetchContent (pinned in `CMakeLists.txt`); it is not a sibling checkout or submodule.

## Prerequisites

- CMake 3.20 or newer
- Ninja or Visual Studio generators
- `VCPKG_ROOT` set to a working vcpkg installation
- vcpkg manifest dependencies available for `protobuf`, `yaml-cpp`, and `gtest`
- sibling source repos for `CRUMBS` and `bread-crumbs-contracts`

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

Install Visual Studio 2022 (or Build Tools) with the `Desktop development with C++` workload.

Install vcpkg:

```powershell
git clone https://github.com/microsoft/vcpkg.git $env:USERPROFILE\vcpkg
& "$env:USERPROFILE\vcpkg\bootstrap-vcpkg.bat"
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", "$env:USERPROFILE\\vcpkg", "User")
$env:VCPKG_ROOT = [Environment]::GetEnvironmentVariable("VCPKG_ROOT", "User")
Test-Path "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
```

## Configure And Build

One binary per platform covers both hardware and no-hardware operation. Hardware
support is always compiled into the Linux binary; whether the provider touches
real hardware is decided at runtime by `hardware.bus_path` in the config (see
[Running The Provider Manually](#running-the-provider-manually)), not by a build
flag. The presets below build and test without real hardware on all platforms,
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
builds above already include hardware capability on Linux (`linux-wire` is always
compiled in, alongside the sibling `CRUMBS` and `bread-crumbs-contracts` repos).
To work against real CRUMBS and BREAD hardware, run that same binary with a config
whose `hardware.bus_path` points at a real device node (e.g. `/dev/i2c-1`); any
non-`mock://` path opens a live CRUMBS session.

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

The committed sample config seeds one RLHT and one DCMT device so `Hello`, `WaitReady`,
`ListDevices`, `DescribeDevice`, and `GetHealth` are testable without real hardware.

To exercise the config-seeded inventory without hardware, set `hardware.bus_path` to a
`mock://...` value (as in `config/ci.test.yaml`). A real bus path that cannot be opened
makes startup fail fast rather than silently falling back to a seeded inventory.

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
| `ci-linux-release` | Yes | Automated CI on every push and PR (Linux) |
| `ci-windows-release` | Yes | Automated CI on every push and PR (Windows) |

The `ci-linux-release` and `ci-windows-release` lanes run automatically via GitHub Actions
(`.github/workflows/ci.yml`). They check out `CRUMBS` and `bread-crumbs-contracts` as sibling
directories and exercise the provider against a `mock://` bus (no I2C hardware).

There is no automated hardware lane. Real-hardware validation is performed manually by running
the released binary on the Linux host with a real `hardware.bus_path` pointing at the CRUMBS bus.

## Notes

- `CRUMBS` and `bread-crumbs-contracts` are first-class source dependencies for all builds.
- Hardware vs config-seeded operation is selected at runtime by `hardware.bus_path` (`mock://...` vs a real path), not by a build flag.
- On Linux, `linux-wire` is always compiled into the provider and is added before `CRUMBS`.
- The CRUMBS session layer (`src/crumbs/`) owns scan/send/read/query-read behavior only.
- Inventory and compatibility code (`src/devices/common/`) uses `bread-crumbs-contracts` for type
  IDs, baseline capability profiles, and compatibility rules.
