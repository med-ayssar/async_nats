{
  description = "async_nats";

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
        system: compiler:
        let
          pkgs = pkgsFor system compiler;
          stdenv = stdenvOf pkgs compiler;
          libraries = import ./libraries {
            inputs = { };
            inherit system compiler;
          };
        in
        pkgs.callPackage ./package.nix {
          inherit stdenv libraries;
          boost = pkgs.boost190;
          spdlog = pkgs.spdlog;
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
          ]
          ++ builtins.attrValues libraries;
        };
    in
    flake-utils.lib.eachDefaultSystem (
      system: {
        packages = {
          default = packageFor system "clang";
          clang = packageFor system "clang";
          gcc = packageFor system "gcc";
        };
        devShells = {
          default = shellFor system "clang";
          clang = shellFor system "clang";
          gcc = shellFor system "gcc";
        };
      }
    )
    // {
      overlays.default = boostOverlay;
    };
}
