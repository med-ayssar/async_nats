#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPILER="clang"
BUILD_TYPE="debug"

usage() {
  echo "Usage: $0 [--clang|--gcc] [--debug|--release]"
  echo "  Install AsyncNats into the Nix profile."
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
nix profile add "${SCRIPT_DIR}/result"

echo "${BUILD_TYPE} library installed."
echo "Store output: ${SCRIPT_DIR}/result"
echo "Profile:      ${HOME}/.nix-profile/lib/libAsyncNats.a"
echo "Headers:      ${HOME}/.nix-profile/include/AsyncNats.h"
