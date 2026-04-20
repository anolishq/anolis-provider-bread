# Contributing

## Workspace Setup

This repo depends on sibling source repos at the same directory level.
Clone them all before configuring:

```bash
cd repos_feast  # or wherever you keep repos
git clone https://github.com/anolishq/anolis-provider-bread
git clone https://github.com/FEASTorg/CRUMBS
git clone https://github.com/FEASTorg/bread-crumbs-contracts
git clone https://github.com/anolishq/linux-wire
```

Then initialize the `anolis-protocol` submodule inside `anolis-provider-bread`:

```bash
cd anolis-provider-bread
git submodule update --init --recursive
```

Expected layout:

```text
repos_feast/
├── anolis-provider-bread/
│   └── external/anolis-protocol/   ← submodule
├── CRUMBS/
├── bread-crumbs-contracts/
└── linux-wire/
```

vcpkg must be installed and `VCPKG_ROOT` set before configuring.
See [docs/build.md](docs/build.md) for the full prerequisites list.

---

## Build And Test

All standard workflows use CMake presets.

### No Hardware (runs everywhere)

```bash
cmake --preset dev-debug
cmake --build --preset dev-debug
ctest --preset dev-debug
```

On Windows with MSVC, use the `-windows-` variants:

```powershell
cmake --preset dev-windows-debug
cmake --build --preset dev-windows-debug
ctest --preset dev-windows-debug
```

### Linux hardware path

```bash
cmake --preset dev-linux-hardware-debug
cmake --build --preset dev-linux-hardware-debug
ctest --preset dev-linux-hardware-debug
```

This requires the sibling `linux-wire` repo and a real I2C bus.

### Validating a config without running the provider

```bash
./build/dev-debug/anolis-provider-bread --check-config config/example.local.yaml
```

---

## Repo Conventions

- **`docs/`** — committed, stable documentation. Keep it short and accurate.
- **`working/`** — planning notes and scratch. Not tracked by git.
- **`config/`** — example configs committed to the repo. The `*.local.yaml` pattern is gitignored for machine-specific overrides.
- Log messages are intended for operators and developers, not for programmatic parsing. Message text is not stable across releases.

---

## Adding A New Device Type

A BREAD device type requires changes in five places.

### 1. `bread-crumbs-contracts` (separate repo)

The new type's wire contract (`type_id`, version constants, opcode table, capability flags, signal and function payload layouts) belongs in `bread-crumbs-contracts` as a new header pair.
This change must land in `bread-crumbs-contracts` before the provider can reference it.

### 2. `src/devices/common/bread_compatibility.hpp/.cpp`

- Add the new type to `DeviceType` enum.
- Add `try_parse_bread_type`: map the new `TYPE_ID` constant to the new enum value.
- Add `bread_type_id`: reverse mapping.
- Add `bread_contract_name` and `provider_type_id` string labels.
- Add `expected_module_version`: return the expected `{major, minor}` from the contracts constants.
- Add `make_baseline_capability_profile`: return the safe conservative fallback for the new type.

### 3. `src/devices/<newtype>/`

Create a new subdirectory with `<newtype>_adapter.hpp` and `<newtype>_adapter.cpp`.

The adapter must implement:

- `describe_device(const inventory::DeviceRecord &)` → `anolis::deviceprovider::v1::Device`
- `read_signals(crumbs::Session &, const inventory::DeviceRecord &, ...)` → signal values
- `call(crumbs::Session &, const inventory::DeviceRecord &, ...)` → call result

Follow the structure of `src/devices/rlht/rlht_adapter.cpp` or `src/devices/dcmt/dcmt_adapter.cpp` as a reference.

### 4. `src/core/handlers.cpp`

Wire the new adapter into `handle_list_devices`, `handle_describe_device`, `handle_read_signals`, and `handle_call` dispatch logic.

### 5. Tests

Add a `tests/unit/<newtype>_adapter_test.cpp` covering at minimum:

- Signal read with all capability variants
- Function call for each supported function
- Call/read failure path (adapter error propagation)

Use `GoogleTest`. Look at `dcmt_adapter_test.cpp` or `rlht_adapter_test.cpp` as a reference.

---

## Cross-Repo Changes

Changes that touch `bread-crumbs-contracts` and this provider together:

1. Land the contracts change in `bread-crumbs-contracts` first (PR + merge).
2. Update the workspace to point at the new contracts commit.
3. Update any compatibility constants, adapter code, or capability handling in this repo.
4. If the contracts change bumps a module major version, treat the provider change as a major version bump.

The `external/anolis-protocol` submodule should only be advanced if the consuming runtime (anolis) has already adopted the new protocol version.

---

## Debugging Provider/Runtime/Hardware Interactions

All provider log output goes to stderr.
The ADPP framed-stdio transport uses stdout.
Redirect them separately when debugging:

```bash
./anolis-provider-bread --config config/example.local.yaml 2>provider.log
```

To watch logs while piping the provider into a runtime or test harness:

```bash
./anolis-provider-bread --config config/example.local.yaml 2>&1 1>/dev/null
```

For hardware path issues, match the `[WARN]` / `[ERROR]` probe lines against [docs/troubleshooting.md](docs/troubleshooting.md).

The no-hardware build (`ANOLIS_PROVIDER_BREAD_ENABLE_HARDWARE=OFF`) runs without I2C hardware.
If a hardware-path behavior is hard to reproduce, first verify the same path through the no-hardware build with a config-seeded inventory before debugging hardware specifically.
