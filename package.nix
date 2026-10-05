{
  lib,
  stdenv,
  cmake,
  ninja,
  boost,
  spdlog,
  libraries,
  catch2_3,
  withTests ? false,
  buildType ? "Release",
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
