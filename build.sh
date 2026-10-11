#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPILER="clang"
BUILD_TYPE="debug"

usage() {
  echo "Usage: $0 [--clang|--gcc] [--debug|--release]"
  echo "  Build and install AsyncNats with the active toolchain. C++23."
  echo "  --clang    Clang (default)"
  echo "  --gcc      GCC"
  echo "  --debug    Debug (default)"
  echo "  --release  Release"
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
  --clang)
    COMPILER="clang"
    shift
    ;;
  --gcc)
    COMPILER="gcc"
    shift
    ;;
  --release)
    BUILD_TYPE="release"
    shift
    ;;
  --debug)
    BUILD_TYPE="debug"
    shift
    ;;
  -h | --help)
    usage
    ;;
  *)
    echo "Error: Unknown argument '$1'"
    usage
    ;;
  esac
done

cd "${SCRIPT_DIR}"

# Configure, build, and install with the active toolchain.
BUILD_DIR="${SCRIPT_DIR}/build/${COMPILER}"
INSTALL_PREFIX="${SCRIPT_DIR}/install"
CXX_COMPILER="clang++"
if [[ "${BUILD_TYPE}" == "debug" ]]; then
  CMAKE_BUILD_TYPE="Debug"
else
  CMAKE_BUILD_TYPE="Release"
fi
if [[ "${COMPILER}" == "gcc" ]]; then
  CXX_COMPILER="g++"
fi

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" \
  -DCMAKE_CXX_COMPILER="${CXX_COMPILER}" \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "${BUILD_DIR}" --parallel
cmake --install "${BUILD_DIR}"

ln -sfn "build/${COMPILER}/compile_commands.json" "${SCRIPT_DIR}/compile_commands.json"
echo "Compile commands:  ${SCRIPT_DIR}/compile_commands.json"
echo "Install prefix:    ${INSTALL_PREFIX}"
echo "Library:           ${INSTALL_PREFIX}/lib/libAsyncNats.a"
echo "CMake package:     ${INSTALL_PREFIX}/lib/cmake/AsyncNats/"
