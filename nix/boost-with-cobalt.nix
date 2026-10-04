# Nixpkgs Boost does not build Cobalt. The Cobalt Jamfile requires
# cxx20_hdr_concepts, and the nixpkgs b2 invocation never passes cxxstd,
# so the library is skipped. This builds Cobalt with C++20 through the
# same derivation, which keeps its CMake files beside BoostConfig.
#
# Keep this file identical in async_nats and superbuild so both flakes
# produce the same Boost store path.
#
# Cobalt's main.cpp defines main(). async_nats provides main, and a shared
# library must not export another one.
{
  lib,
  boost,
}:
(boost.override {
  extraB2Args = [ "cxxstd=20" ];
}).overrideAttrs (
  old:
  let
    # nixpkgs always passes --without-python. b2 rejects that together with
    # --with-<library>, so swap it for the libraries this package needs.
    # BoostConfig requires the headers component. Cobalt's shared library
    # also installs container, context, and date_time.
    withCobalt = lib.replaceStrings [ "--without-python" ] [
      "--with-headers --with-cobalt --with-container --with-context --with-date_time"
    ];
  in
  {
    buildPhase = withCobalt old.buildPhase;
    installPhase = withCobalt old.installPhase;
    postPatch = (old.postPatch or "") + ''
      for jam in libs/cobalt/build/Jamfile libs/cobalt/build/Jamfile.v2; do
        if [ -f "$jam" ]; then
          sed -i.bak '/main\.cpp/d' "$jam"
          rm -f "$jam.bak"
        fi
      done
    '';

    # CMake files are still in $out here. The multi-output fixup moves
    # lib/cmake to $dev afterwards.
    postInstall = (old.postInstall or "") + ''
      if [ ! -e "$dev/include/boost/asio.hpp" ] && [ ! -e "$out/include/boost/asio.hpp" ]; then
        mkdir -p "$dev/include"
        cp -a boost "$dev/include/"
      fi
      if [ ! -d "$out/lib/cmake/boost_cobalt-1.91.0" ] && [ ! -d "$dev/lib/cmake/boost_cobalt-1.91.0" ]; then
        echo "boost_cobalt CMake package was not installed" >&2
        ls -la "$out/lib/cmake" "$dev/lib/cmake" >&2 || true
        exit 1
      fi
      if [ ! -d "$out/lib/cmake/boost_headers-1.91.0" ] && [ ! -d "$dev/lib/cmake/boost_headers-1.91.0" ]; then
        echo "boost_headers CMake package was not installed" >&2
        ls -la "$out/lib/cmake" >&2 || true
        exit 1
      fi
      if [ ! -e "$out/lib/libboost_cobalt.dylib" ] && [ ! -e "$out/lib/libboost_cobalt.so" ]; then
        echo "libboost_cobalt was not installed" >&2
        ls -la "$out/lib" >&2 || true
        exit 1
      fi
      if [ ! -e "$dev/include/boost/cobalt/spawn.hpp" ] && [ ! -e "$out/include/boost/cobalt/spawn.hpp" ]; then
        echo "Cobalt headers were not installed" >&2
        exit 1
      fi
    '';
  }
)
