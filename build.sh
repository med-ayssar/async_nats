#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="${SCRIPT_DIR}/install"
FRESH_BUILD=false

usage() {
  echo "Usage: $0 [-p|--path <install_path>] [-f|--fresh]"
  echo "  Build and install the async_nats library."
  echo "  -p, --path   Installation directory (default: ./install)"
  echo "  -f, --fresh  Remove ./build before configuring"
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
  -p | --path)
    INSTALL_DIR="$2"
    shift 2
    ;;
  -f | --fresh)
    FRESH_BUILD=true
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

if [ "${FRESH_BUILD}" = true ]; then
  rm -rf build
fi

echo "[1/4] Conan dependencies..."
conan install . --build=missing

echo "[2/4] Configure..."
cmake -B build/Debug -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="${SCRIPT_DIR}/build/Release/generators/conan_toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}"

ln -sfn build/Debug/compile_commands.json compile_commands.json

echo "[3/4] Build async_nats..."
cmake --build build/Debug --target async_nats

echo "[4/4] Install to ${INSTALL_DIR}..."
cmake --install build/Debug --component library

echo "Installed library: ${INSTALL_DIR}/lib/libasync_nats.a"
echo "CMake package:    ${INSTALL_DIR}/lib/cmake/async_nats/"
