# anolis-provider-bread

[![CI](https://github.com/FEASTorg/anolis-provider-bread/actions/workflows/ci.yml/badge.svg)](https://github.com/FEASTorg/anolis-provider-bread/actions/workflows/ci.yml)

BREAD hardware provider for the Anolis runtime.

Implements the Anolis Device Provider Protocol (ADPP) v1 over BREAD-over-CRUMBS, exposing RLHT
and DCMT devices to the Anolis runtime via framed stdio. Supports Linux hardware integration and a
no-hardware build for development and CI.

Repository documentation:

- [docs/README.md](docs/README.md) — scope, design decisions, key rules
- [docs/build.md](docs/build.md) — dependency install, configure, build, and test workflow
- [docs/troubleshooting.md](docs/troubleshooting.md) — common failure modes and log signatures
- [docs/versioning.md](docs/versioning.md) — provider and dependency version expectations
- [CONTRIBUTING.md](CONTRIBUTING.md) — workspace setup, adding device types, cross-repo workflow

Local API docs: run `doxygen docs/Doxyfile` from the repo root.
Generated output goes to `build/docs/doxygen/html/` and remains untracked.
