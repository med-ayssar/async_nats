{
  description = "async_nats";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "aarch64-darwin"
        "aarch64-linux"
        "x86_64-darwin"
        "x86_64-linux"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems f;
      stdenvOf =
        pkgs: compiler:
        if compiler == "gcc" then pkgs.gccStdenv else pkgs.clangStdenv;
      depsFor =
        pkgs: stdenv:
        let
          sameStdenv = stdenv == pkgs.stdenv;
        in
        {
          boost = pkgs.callPackage ./nix/boost-with-cobalt.nix {
            boost = if sameStdenv then pkgs.boost190 else pkgs.boost190.override { inherit stdenv; };
          };
          spdlog = if sameStdenv then pkgs.spdlog else pkgs.spdlog.override { inherit stdenv; };
        };
      packageFor =
        pkgs: system: compiler:
        let
          stdenv = stdenvOf pkgs compiler;
          deps = depsFor pkgs stdenv;
          libraries = import ./libraries {
            inputs = { };
            inherit system compiler;
          };
        in
        pkgs.callPackage ./package.nix {
          inherit stdenv libraries;
          inherit (deps) boost spdlog;
        };
      shellFor =
        pkgs: system: compiler:
        let
          stdenv = stdenvOf pkgs compiler;
          deps = depsFor pkgs stdenv;
          libraries = import ./libraries {
            inputs = { };
            inherit system compiler;
          };
        in
        (pkgs.mkShell.override { inherit stdenv; }) {
          packages = [
            pkgs.cmake
            pkgs.ninja
            deps.boost
            deps.spdlog
          ]
          ++ builtins.attrValues libraries;
        };
    in
    {
      packages = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = packageFor pkgs system "clang";
          clang = packageFor pkgs system "clang";
          gcc = packageFor pkgs system "gcc";
        }
      );

      devShells = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = shellFor pkgs system "clang";
          clang = shellFor pkgs system "clang";
          gcc = shellFor pkgs system "gcc";
        }
      );
    };
}
