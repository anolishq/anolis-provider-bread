# Scripts

Helper scripts for common build and test workflows.

| Script | Purpose |
|--------|---------|
| `build.sh` | Configure and build via CMake preset (Linux/macOS) |
| `build.ps1` | Configure and build via CMake preset (Windows/cross-platform) |
| `test.sh` | Run CTest suite via preset (Linux/macOS) |
| `test.ps1` | Run CTest suite via preset (Windows/cross-platform) |

Each script supports `--help` for usage details.

Default preset on Linux/macOS: `dev-debug`.
Default preset on Windows: `dev-windows-debug`.

Test suites: `all` (default), `unit`, `shell`.
