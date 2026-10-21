#!/bin/bash
set -euo pipefail

pushd "$(dirname "$(realpath -s "$0")")/.." > /dev/null

if [[ -z "${NUM_CPUS:-}" ]]; then
    NUM_CPUS=$(($(nproc) * 2))
fi

BUILD_DIR="${BUILD_DIR:-build}"
PAYLOAD_SDK_DIR="${PAYLOAD_SDK_DIR:-../Payload-SDK/}"
PSDK_TARGET_OS="${PSDK_TARGET_OS:-linux}"

PSDK_PLATFORM="${PSDK_PLATFORM:-raspberry_pi}"

CMAKE_ARGS=(
    -S .
    -B "${BUILD_DIR}"
    -DPAYLOAD_SDK_DIR="${PAYLOAD_SDK_DIR}"
    -DPSDK_PLATFORM="${PSDK_PLATFORM}"
    -DPSDK_TARGET_OS="${PSDK_TARGET_OS}"
)

echo "Using ${BUILD_DIR}, ${PSDK_PLATFORM}, ${PSDK_TARGET_OS}"
if [[ -n "${PSDK_TOOLCHAIN_NAME:-}" ]]; then
    CMAKE_ARGS+=(-DPSDK_TOOLCHAIN_NAME="${PSDK_TOOLCHAIN_NAME}")
fi

if [[ -n "${CMAKE_TOOLCHAIN_FILE:-}" ]]; then
    CMAKE_ARGS+=(-DCMAKE_TOOLCHAIN_FILE="${CMAKE_TOOLCHAIN_FILE}")
fi

if [[ -n "${CMAKE_C_COMPILER:-}" ]]; then
    CMAKE_ARGS+=(-DCMAKE_C_COMPILER="${CMAKE_C_COMPILER}")
fi

if [[ -n "${CMAKE_BUILD_TYPE:-}" ]]; then
    CMAKE_ARGS+=(-DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
fi

cmake "${CMAKE_ARGS[@]}" "$@"
cmake --build "${BUILD_DIR}" -j"${NUM_CPUS}"

popd > /dev/null
