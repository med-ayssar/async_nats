{
  description = "AsyncNats";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
    }:
    let
      # Nixpkgs Boost 1.90 has Cobalt headers and no libboost_cobalt.
      # Replace pkgs.boost190 with the C++23 Cobalt build.
      boostOverlay = final: prev: {
        boost190 = prev.callPackage ./nix/boost-with-cobalt.nix {
          boost = prev.boost190;
        };
      };
      stdenvOf =
        prev: compiler:
        if compiler == "gcc" then prev.gccStdenv else prev.clangStdenv;
      pkgsFor =
        system: compiler:
        import nixpkgs {
          inherit system;
          overlays = [
            (final: prev:
              let
                stdenv = stdenvOf prev compiler;
              in
              {
                boost190 =
                  if stdenv == prev.stdenv then prev.boost190
                  else prev.boost190.override { inherit stdenv; };
                spdlog =
                  if stdenv == prev.stdenv then prev.spdlog
                  else prev.spdlog.override { inherit stdenv; };
              })
            boostOverlay
          ];
        };
      packageFor =
        system: compiler: buildType: withTests:
        let
          pkgs = pkgsFor system compiler;
          stdenv = stdenvOf pkgs compiler;
          libraries = import ./libraries {
            inputs = { };
            inherit system compiler;
          };
          cmakeBuildType = if buildType == "debug" then "Debug" else "Release";
        in
        pkgs.callPackage ./package.nix {
          inherit stdenv libraries withTests;
          buildType = cmakeBuildType;
          boost = pkgs.boost190;
          spdlog = pkgs.spdlog;
          catch2_3 = pkgs.catch2_3;
        };
      shellFor =
        system: compiler:
        let
          pkgs = pkgsFor system compiler;
          stdenv = stdenvOf pkgs compiler;
          libraries = import ./libraries {
            inputs = { };
            inherit system compiler;
          };
        in
        (pkgs.mkShell.override { inherit stdenv; }) {
          packages = [
            pkgs.cmake
            pkgs.ninja
            pkgs.boost190
            pkgs.spdlog
            pkgs.catch2_3
          ]
          ++ builtins.attrValues libraries;
        };
    in
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        clangDebug = packageFor system "clang" "debug" false;
        clangRelease = packageFor system "clang" "release" false;
        gccRelease = packageFor system "gcc" "release" false;
        tests = packageFor system "clang" "release" true;
        docPkgs = import nixpkgs { inherit system; };
        doc = docPkgs.callPackage ./nix/doc.nix { };
      in
      {
        packages = {
          default = clangDebug;
          doc = doc;
          "async-nats-clang-release" = clangRelease;
          "async-nats-clang-debug" = clangDebug;
          "async-nats-gcc-release" = gccRelease;
          "async-nats-gcc-debug" = packageFor system "gcc" "debug" false;
          "async-nats-tests" = tests;
          # Short names used by docktopus: packages.${system}.${compiler}
          clang = clangRelease;
          gcc = gccRelease;
          tests = tests;
        };
        devShells = {
          default = shellFor system "clang";
          "async-nats-clang" = shellFor system "clang";
          "async-nats-gcc" = shellFor system "gcc";
          clang = shellFor system "clang";
          gcc = shellFor system "gcc";
          doc = docPkgs.mkShell {
            packages = [
              docPkgs.nodejs
              doc
            ];
          };
        };
      }
    )
    // {
      overlays.default = boostOverlay;
    };
}
