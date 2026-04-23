# anolis-provider-bread

[![CI](https://github.com/anolishq/anolis-provider-bread/actions/workflows/ci.yml/badge.svg)](https://github.com/anolishq/anolis-provider-bread/actions/workflows/ci.yml)

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

## Quick Start

### Download and run (recommended)

Download the latest release binary (Linux x86_64):

```bash
VERSION=$(curl -fsSL https://api.github.com/repos/anolishq/anolis-provider-bread/releases/latest | grep '"tag_name"' | sed 's/.*"v\([^"]*\)".*/\1/')
curl -fsSL "https://github.com/anolishq/anolis-provider-bread/releases/download/v${VERSION}/anolis-provider-bread-${VERSION}-linux-x86_64.tar.gz" \
  | tar -xz
# binary is at ./bin/anolis-provider-bread
```

Provider-bread is started by the Anolis runtime as a subprocess — it is not run directly.
Point your [`anolis`](https://github.com/anolishq/anolis/releases/latest) runtime config at it:

```yaml
# in your anolis-runtime.yaml providers section:
providers:
  - id: bread0
    command: ./bin/anolis-provider-bread
    args: ["--config", "./providers/bread0.yaml"]
    timeout_ms: 5000
```

A sample provider config is at `config/example.local.yaml` in the source. See
[docs/README.md](docs/README.md) for device types, signal surfaces, and hardware wiring.

### Build from source (contributors / hardware builds)

Linux hardware builds require CRUMBS and linux-wire as sibling checkouts. See
[docs/build.md](docs/build.md) for the full dependency install and configure workflow.

No-hardware build (dev/CI):

```bash
git clone https://github.com/anolishq/anolis-provider-bread.git
cd anolis-provider-bread
git clone https://github.com/feastorg/CRUMBS.git ../CRUMBS
git clone https://github.com/feastorg/bread-crumbs-contracts.git ../bread-crumbs-contracts
cmake --preset dev-debug \
  -DCRUMBS_DIR=../CRUMBS \
  -DBREAD_CONTRACTS_DIR=../bread-crumbs-contracts
cmake --build --preset dev-debug --parallel
ctest --preset dev-debug
```

Linux hardware build:

```bash
cmake --preset dev-linux-hardware-release
cmake --build --preset dev-linux-hardware-release --parallel
```


