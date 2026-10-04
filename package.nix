{
  lib,
  stdenv,
  cmake,
  ninja,
  boost,
  spdlog,
  libraries,
}:
stdenv.mkDerivation {
  pname = "async_nats";
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

  cmakeFlags = [
    "-DCMAKE_CXX_STANDARD=23"
    "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
    "-DCMAKE_CXX_EXTENSIONS=OFF"
    "-DASYNC_NATS_BUILD_SAMPLE=OFF"
  ];

  meta = {
    description = "Async NATS client runtime. The library owns main and runs co_main.";
    license = lib.licenses.mit;
  };
}
