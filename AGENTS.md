# AGENTS.md — anolis-provider-bread

> Per-repo conventions for coding agents (Claude Code, OpenCode, …). The
> canonical cross-repo rules — Conventional Commits, minimal-first/YAGNI, no
> secrets, run checks before asserting success — live in the user's **global**
> `AGENTS.md` and are not repeated here. This file records only what is
> **specific to this repo**. Backlog lives in GitHub issues, **not** a TODO.md.

## Build / test

- C++20 (`std::format` for diagnostics). Build and test via presets:
  `cmake --preset ci-linux-release` → `cmake --build --preset ci-linux-release`
  → `ctest --preset ci-linux-release`. `just check && just test` wraps this.
- The required CI status check is the **`ok`** job (it aggregates the lanes);
  never bypass it, and never merge red.

## Tooling

- clang-format / clang-tidy are pinned to **18.1.8** via the shared
  `setup-clang-tools` action — do NOT use pip/apt/pre-commit/container versions.
  Run `clang-format -i` before **every** commit (CI fails otherwise).
  `tests/.clang-tidy` relaxes test-only check noise; production checks still apply.
- Shared `.github` actions/workflows are SHA-pinned with a `# <tag>` comment so
  Renovate can track them — keep that comment when bumping.

## Repo-specific gotchas

- **Mock vs hardware is chosen at runtime by the `mock://` `bus_path` in config,
  NOT a build flag.** In mock mode a `MockTransport`
  (`src/crumbs/mock_transport.*`) produces real read values, so reads/calls work
  without hardware.
- Devices ride the **CRUMBS** bus. Device types — **RLHT** (relay + temp) and
  **DCMT** (dual motor) — come from `bread-crumbs-contracts`. Type dispatch
  happens only in `adapter_for()` (`src/devices/common/`), a `-Werror=switch`
  exhaustive switch: **adding a device type means updating that switch** (plus a
  contracts header, compatibility entry, adapter, and tests — see CONTRIBUTING.md).
- Conformance runs via `--provider-profile config/conformance.toml`. One known
  skip is §8.1 (no zero-arg result-producing function) — a contract gap, not a
  bug, so don't "fix" it here.
