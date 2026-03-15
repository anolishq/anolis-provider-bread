# Build Notes

Phase 0 establishes the build and dependency foundation for `anolis-provider-bread`.

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

Use the foundation path when you only want the config and protocol scaffolding.

On Windows with MSVC:

```powershell
cmake --preset dev-windows-foundation-debug
cmake --build --preset dev-windows-foundation-debug
ctest --preset dev-windows-foundation-debug
```

On non-Windows hosts:

```powershell
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

## Config Check

The Phase 0 executable only validates config loading and the generated ADPP bindings.

PowerShell:

```powershell
.\build\dev-windows-foundation-debug\Debug\anolis-provider-bread.exe --check-config config\example.local.yaml
```

Non-Windows single-config builds:

```bash
./build/dev-foundation-debug/anolis-provider-bread --check-config config/example.local.yaml
```

## Notes

- Hardware integration is intentionally gated behind `ANOLIS_PROVIDER_BREAD_ENABLE_HARDWARE`.
- When hardware integration is enabled, the parent build should add `linux-wire` before `CRUMBS`.
- Current BREAD headers include CRUMBS headers directly, so future device adapter targets should link through the local `anolis_provider_bread_bread_contracts` interface target.
