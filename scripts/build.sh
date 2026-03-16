#!/usr/bin/env bash
# Build anolis-provider-bread via CMake presets.
#
# Usage:
#   ./scripts/build.sh [options] [-- <extra-cmake-configure-args>]
#
# Options:
#   --preset <name>   Configure/build preset (default: dev-foundation-debug)
#   --clean           Remove preset build directory before configure
#   -j, --jobs <N>    Parallel build jobs
#   -h, --help        Show help

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

PRESET="dev-foundation-debug"
CLEAN=false
JOBS=""
EXTRA_CONFIG_ARGS=()

usage() {
    sed -n '1,14p' "$0"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
    --preset)
        [[ $# -lt 2 ]] && { echo "[ERROR] --preset requires a value"; exit 2; }
        PRESET="$2"
        shift 2
        ;;
    --preset=*)
        PRESET="${1#*=}"
        shift
        ;;
    --clean)
        CLEAN=true
        shift
        ;;
    -j | --jobs)
        [[ $# -lt 2 ]] && { echo "[ERROR] $1 requires a value"; exit 2; }
        JOBS="$2"
        shift 2
        ;;
    --jobs=*)
        JOBS="${1#*=}"
        shift
        ;;
    -h | --help)
        usage
        exit 0
        ;;
    --)
        shift
        EXTRA_CONFIG_ARGS=("$@")
        break
        ;;
    *)
        echo "[ERROR] Unknown option: $1"
        usage
        exit 2
        ;;
    esac
done

BUILD_DIR="$REPO_ROOT/build/$PRESET"
if [[ "$CLEAN" == true ]]; then
    echo "[INFO] Cleaning build directory: $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

echo "[INFO] Configure preset: $PRESET"
cmake --preset "$PRESET" "${EXTRA_CONFIG_ARGS[@]}"

echo "[INFO] Build preset: $PRESET"
if [[ -n "$JOBS" ]]; then
    cmake --build --preset "$PRESET" --parallel "$JOBS"
else
    cmake --build --preset "$PRESET" --parallel
fi
