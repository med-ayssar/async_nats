#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPILER="clang"
BUILD_TYPE="release"

usage() {
  echo "Usage: $0 [--clang|--gcc] [--debug|--release]"
  echo "  Build async_nats with Nix. C++23."
  echo "  --clang    Clang (default)"
  echo "  --gcc      GCC"
  echo "  --release  Release (default)"
  echo "  --debug    Debug"
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

# The Nix build runs in a sandbox, so its compile database points at that
# temporary tree. Configure the real source tree with the same compiler and
# leave compile_commands.json where clangd looks for it.
BUILD_DIR="${SCRIPT_DIR}/build/${COMPILER}"
nix develop ".#async-nats-${COMPILER}" -c cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DASYNC_NATS_BUILD_SAMPLE=OFF \
  -DBUILD_TESTS=ON
# CMake asks the Nix compiler which include directories it searches on its own.
# It then omits those directories from compile_commands.json. Homebrew clangd
# never runs that Nix compiler, so the omitted paths have to be written back
# into each command as -isystem flags.
python3 - "${BUILD_DIR}" <<'PY'
import json
import re
import sys
from pathlib import Path

build = Path(sys.argv[1])

# CMake records the compiler's own include list in this file, for example:
#   set(CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES "/nix/store/...-boost/include;/nix/store/...-spdlog/include")
compiler = next(build.glob("CMakeFiles/*/CMakeCXXCompiler.cmake"))
text = compiler.read_text()
found = re.search(r'set\(CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES "(.*)"\)', text)
dirs = [item for item in found.group(1).split(";") if item] if found else []

# compile_commands.json is a list of { "file", "command", "directory" }.
# "command" is the full compiler invocation for that source file.
database = build / "compile_commands.json"
entries = json.loads(database.read_text())
for entry in entries:
    command = entry["command"]
    # Skip a directory that is already present, so running the script twice
    # does not add the same -isystem flag again.
    missing = [f"-isystem {include}" for include in dirs if include not in command]
    if not missing:
        continue
    # The command starts with the compiler path. Insert the flags right after it:
    #   clang++ -c client.cpp
    # becomes
    #   clang++ -isystem /nix/store/...-boost/include -c client.cpp
    compiler_path, rest = command.split(" ", 1)
    entry["command"] = compiler_path + " " + " ".join(missing) + " " + rest
database.write_text(json.dumps(entries, indent=2) + "\n")
PY
ln -sfn "build/${COMPILER}/compile_commands.json" "${SCRIPT_DIR}/compile_commands.json"

echo "Installed package: ${SCRIPT_DIR}/result"
echo "Library:           ${SCRIPT_DIR}/result/lib/libasync_nats.a"
echo "CMake package:     ${SCRIPT_DIR}/result/lib/cmake/async_nats/"
echo "Compile commands:  ${SCRIPT_DIR}/compile_commands.json"
