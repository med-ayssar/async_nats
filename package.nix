{
  lib,
  stdenv,
  cmake,
  ninja,
  boost,
  spdlog,
  libraries,
  catch2_3,
  python3,
  withTests ? false,
  buildType ? "Debug",
}:
stdenv.mkDerivation {
  pname = "AsyncNats";
  version = "0.1.0";

  src = lib.cleanSourceWith {
    src = ./.;
    filter =
      path: type:
      let
        base = baseNameOf path;
      in
      !(builtins.elem base [
        "build"
        "install"
        "result"
        ".cache"
        ".git"
      ])
      && lib.cleanSourceFilter path type;
  };

  nativeBuildInputs = [
    cmake
    ninja
  ]
  ++ lib.optionals (buildType == "Debug") [
    python3
  ]
  ++ lib.optionals withTests [
    catch2_3
  ];

  buildInputs = [
    boost
    spdlog
  ]
  ++ builtins.attrValues libraries;

  propagatedBuildInputs = [
    boost
    spdlog
  ]
  ++ builtins.attrValues libraries;

  cmakeBuildType = buildType;

  # Keep the debug symbols in the static archive so an app can step into the library.
  dontStrip = buildType == "Debug";

  # The sandbox compile database names a temporary tree. Copy the sources into
  # the package and rewrite the database so clangd can open those store paths.
  postInstall = lib.optionalString (buildType == "Debug") ''
    python3 - "$NIX_BUILD_TOP" "$out" <<'PY'
import json
import shutil
import sys
from pathlib import Path

build_top = Path(sys.argv[1])
out = Path(sys.argv[2])
databases = list(build_top.rglob("compile_commands.json"))
if len(databases) != 1:
    raise SystemExit(f"expected one compile_commands.json, found {databases}")
entries = json.loads(databases[0].read_text())
marker = "/src/async_nats/"
source_roots = set()
kept = []
for entry in entries:
    path = entry["file"]
    index = path.find(marker)
    if index == -1:
        continue
    source_roots.add(path[:index])
    kept.append(entry)
if len(source_roots) != 1:
    raise SystemExit(f"expected one source root, found {source_roots}")
if not kept:
    raise SystemExit("compile_commands.json has no library sources")
source_root = source_roots.pop()
src_tree = Path(source_root) / "src" / "async_nats"
dest_tree = out / "src" / "async_nats"
shutil.copytree(src_tree, dest_tree)
rewritten = []
for entry in kept:
    text = json.dumps(entry).replace(source_root, str(out))
    item = json.loads(text)
    item["directory"] = str(out)
    rewritten.append(item)
(out / "compile_commands.json").write_text(json.dumps(rewritten, indent=2) + "\n")
PY
  '';

  cmakeFlags = [
    "-DCMAKE_CXX_STANDARD=23"
    "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
    "-DCMAKE_CXX_EXTENSIONS=OFF"
  ]
  ++ lib.optionals withTests [
    "-DBUILD_TESTS=ON"
  ];

  doCheck = withTests;

  checkPhase = ''
    runHook preCheck
    ctest --output-on-failure
    runHook postCheck
  '';

  meta = {
    description = "Async NATS client runtime. The library owns main and runs coMain.";
    license = lib.licenses.mit;
  };
}
