#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPILER="clang"

usage() {
  echo "Usage: $0 [--clang|--gcc]"
  echo "  Build and install async_nats with Nix. C++23."
  echo "  --clang  Clang (default)"
  echo "  --gcc    GCC"
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

nix --extra-experimental-features 'nix-command flakes' build ".#${COMPILER}" -o result

echo "Installed package: ${SCRIPT_DIR}/result"
echo "Library:           ${SCRIPT_DIR}/result/lib/libasync_nats.a"
echo "CMake package:     ${SCRIPT_DIR}/result/lib/cmake/async_nats/"
