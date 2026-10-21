#!/bin/bash
set -euo pipefail

cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
# Unset: install on "rp". Set but empty: build only.
RP_HOST="${RP_HOST-rp}"
export CMAKE_TOOLCHAIN_FILE="${CMAKE_TOOLCHAIN_FILE:-cmake/toolchains/aarch64-linux-gnu.cmake}"
export BUILD_DIR="${BUILD_DIR:-build-rpi4-aarch64}"
RP_REQUIRED_COMPILER="aarch64-linux-gnu-gcc"
RP_COMPILER_PACKAGE="gcc-aarch64-linux-gnu"

if [[ -z "${CC:-}" && -z "${CMAKE_C_COMPILER:-}" ]] && ! command -v "${RP_REQUIRED_COMPILER}" > /dev/null; then
    echo "Missing local cross compiler: ${RP_REQUIRED_COMPILER}" >&2
    echo "Install it with: sudo apt install ${RP_COMPILER_PACKAGE}" >&2
    echo "Or pass a compiler path with: CMAKE_C_COMPILER=/path/to/${RP_REQUIRED_COMPILER} ./scripts/build_rp.sh" >&2
    exit 1
fi

./scripts/build.sh "$@"
file "${BUILD_DIR}/SensorPayload"
if [[ ! -z "${RP_HOST}" ]]; then
    ./scripts/install_target.sh --binary "${BUILD_DIR}/SensorPayload" "$RP_HOST"
fi
