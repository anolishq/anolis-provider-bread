# Build Notes

Phase 1 establishes the ADPP provider shell for `anolis-provider-bread`.
Phase 2 adds the provider-owned CRUMBS session layer that later phases will build on.

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

## Foundation-Only Configure

Use the foundation path when you want the config-backed shell without Linux hardware integration.

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

The committed stub config seeds one RLHT and one DCMT device so `Hello`, `WaitReady`, `ListDevices`, `DescribeDevice`, and `GetHealth` are testable before real hardware work begins.

## Notes

- Hardware integration is intentionally gated behind `ANOLIS_PROVIDER_BREAD_ENABLE_HARDWARE`.
- When hardware integration is enabled, the parent build should add `linux-wire` before `CRUMBS`.
- Current BREAD headers include CRUMBS headers directly, so future device adapter targets should link through the local `anolis_provider_bread_bread_contracts` interface target.
- Phase 2 adds a provider-owned CRUMBS session layer. Foundation builds exercise it through fake-transport unit tests; Linux hardware builds compile the real `linux_transport` adapter.
- `LinuxTransport::bind_device(...)` exists specifically so later phases use `bread-crumbs-contracts` for RLHT/DCMT operations instead of rebuilding those contracts on raw CRUMBS frames.
- In Phase 1, `ReadSignals` and `Call` are intentionally present but return `CODE_UNIMPLEMENTED` after validating device and identifier existence.