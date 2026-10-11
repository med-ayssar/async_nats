#!/usr/bin/env bash
# Source this file from an active Nix development shell.

if [[ -z "${IN_NIX_SHELL:-}" ]]; then
  echo "Enter the Nix shell first with: nix develop" >&2
  return 1 2>/dev/null || exit 1
fi

CLANGD="$(command -v clangd || true)"
if [[ -z "${CXX:-}" ]]; then
  CXX="$(command -v clang++ || command -v g++ || true)"
fi
if [[ -z "${CXX}" || -z "${CLANGD}" ]]; then
  echo "A C++ compiler or clangd is missing from the active Nix shell." >&2
  return 1 2>/dev/null || exit 1
fi

export CXX CLANGD NIX_CFLAGS_COMPILE
if [[ "${CXX}" == */clang++ ]]; then
  CLANGXX="${CXX}"
  export CLANGXX
fi
echo "CXX=${CXX}"
echo "CLANGD=${CLANGD}"
