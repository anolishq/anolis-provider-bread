# Versioning And Compatibility

## Provider Version

The provider is versioned via CMake project version and exposed as the compile-time constant `ANOLIS_PROVIDER_BREAD_VERSION`.

| Field | Value |
|-------|-------|
| Current version | `0.1.0` |
| Scheme | [Semantic Versioning](https://semver.org) |

### What Requires A Version Bump

| Change type | Version component |
|-------------|-------------------|
| Incompatible change to ADPP exposure (removed device type, removed function, changed signal type) | Major |
| Added ADPP capability (new supported device type, new optional function) | Minor |
| Bug fix, logging improvement, config schema addition that is backward-compatible | Patch |
| Build system or CI change with no behavioral impact | Patch or nothing |

Hardware behavior changes (new capability flags, updated BREAD firmware expectations) follow the same rule: if an existing well-formed config can no longer produce the same behavior, that is a breaking change.

---

## ADPP Protocol Compatibility

The provider implements the ADPP protocol defined in `external/anolis-protocol`.

| Field | Value |
|-------|-------|
| Proto package | `anolis.deviceprovider.v1` |
| Proto syntax | proto3 |

The `v1` package identifier is the stable protocol generation.
A change to a different package (e.g. `v2`) would require explicit provider support and a major version bump.
The current `external/anolis-protocol` submodule pin is the supported revision.
Do not advance the submodule without verifying that the runtime consuming this provider supports the same revision.

---

## CRUMBS Compatibility

CRUMBS is the I2C transport layer used on the hardware path.

| Field | Value |
|-------|-------|
| Minimum CRUMBS version (firmware) | `1200` (from `BREAD_MIN_CRUMBS_VERSION` in `bread-crumbs-contracts`) |
| CRUMBS host library | sibling repo `CRUMBS/` at the pinned vcpkg or workspace revision |

A device whose firmware reports a CRUMBS version below the minimum is rejected at probe time with `IncompatibleCrumbsVersion`.
See [troubleshooting.md](troubleshooting.md) for the associated log signature.

---

## BREAD Contracts Compatibility

The BREAD device contracts are defined in `bread-crumbs-contracts` (sibling repo).
This provider pins a specific revision at build time via the workspace layout and CMake `find_package`.

| Device | Expected major | Expected minor |
|--------|----------------|----------------|
| RLHT | 1 | 0 |
| DCMT | 1 | 0 |

These values come from `RLHT_MODULE_VER_MAJOR/MINOR` and `DCMT_MODULE_VER_MAJOR/MINOR` in the contracts headers.
Any device whose firmware reports a different major version, or a minor version below the expected minimum, is rejected at probe time.

### Advancing The Contracts Revision

When `bread-crumbs-contracts` releases a new version:

- If only `MINOR` or `PATCH` changed: update the workspace pin, rebuild, run tests. Should be non-breaking.
- If `MAJOR` changed: inspect the change log. Adapter code (`src/devices/`) may need updating. Treat as a potentially breaking change to this provider.

---

## What Does Not Have A Stability Guarantee

| Item | Status |
|------|--------|
| Internal C++ API (adapter interfaces, session layer) | No stability guarantee — private to this repo |
| Log message text | May change in any release — do not parse log lines programmatically |
| Config YAML schema | Additive changes are non-breaking; removed or renamed keys are patch or minor |
| Health metrics keys | Additive changes are non-breaking; removed keys are minor or major depending on whether the runtime depends on them |

---

## Checking Versions At Runtime

The `GetHealth` RPC metrics include the provider inventory mode and device count but not the version string directly. The version is printed to stderr at startup:

```
[INFO] starting with config: ...
```

The full version string is embedded in the binary as `ANOLIS_PROVIDER_BREAD_VERSION` (`0.1.0` in the current build).
Device firmware versions are logged per-device during probe:

```
[INFO] probe 0x08 type=0x01 crumbs=1200 module=1.0.0 caps_source=Queried
```
