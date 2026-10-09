#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPILER="clang"
BUILD_TYPE="debug"

usage() {
  echo "Usage: $0 [--clang|--gcc] [--debug|--release]"
  echo "  Build AsyncNats with Nix. C++23."
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

nix build ".#async-nats-${COMPILER}-${BUILD_TYPE}" -o result

echo "Installed package: ${SCRIPT_DIR}/result"
echo "Library:           ${SCRIPT_DIR}/result/lib/libAsyncNats.a"
echo "CMake package:     ${SCRIPT_DIR}/result/lib/cmake/AsyncNats/"

if [[ "${BUILD_TYPE}" != "debug" ]]; then
  exit 0
fi

# The Nix build runs in a sandbox, so its compile database points at that
# temporary tree. A debug build configures this checkout and leaves
# compile_commands.json where clangd looks for it.
BUILD_DIR="${SCRIPT_DIR}/build/${COMPILER}"
nix develop ".#async-nats-${COMPILER}" -c cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DBUILD_TESTS=ON
# CMake puts the Nix libc++ and NIX_CFLAGS_COMPILE directories back on this
# file. Homebrew clangd reads them directly, so std, Boost, and spdlog resolve.
ln -sfn "build/${COMPILER}/compile_commands.json" "${SCRIPT_DIR}/compile_commands.json"
echo "Compile commands:  ${SCRIPT_DIR}/compile_commands.json"
