# Nixpkgs Boost does not build Cobalt: its Jamfile requires cxx20_hdr_concepts
# and the nixpkgs b2 invocation never passes cxxstd. Build Cobalt, and the
# libraries its CMake package depends on, with C++23.
#
# overlays.default applies this to pkgs.boost190. docktopus uses that
# overlay, so both flakes produce the same Boost store path.
#
# Cobalt's main.cpp defines main(). async_nats provides main, so the shared
# library must not export another one. The io and io_ssl libraries are not
# used.
{
  lib,
  boost,
}:
let
  needed = "--with-headers --with-cobalt --with-container --with-context --with-date_time";
  onlyNeeded = lib.replaceStrings [ "--without-python" ] [ needed ];
in
(boost.override {
  extraB2Args = [ "cxxstd=23" ];
}).overrideAttrs (old: {
  buildPhase = onlyNeeded old.buildPhase;
  installPhase = onlyNeeded old.installPhase;

  postPatch = (old.postPatch or "") + ''
    for jam in libs/cobalt/build/Jamfile libs/cobalt/build/Jamfile.v2; do
      if [ -f "$jam" ]; then
        sed -i.bak '/main\.cpp/d' "$jam"
        rm -f "$jam.bak"
      fi
    done
    sed -i.bak \
      -e 's/alias all : boost_cobalt boost_cobalt_io test example/alias all : boost_cobalt test example/' \
      -e 's/install boost_cobalt boost_cobalt_io boost_cobalt_io_ssl/install boost_cobalt/' \
      libs/cobalt/build.jam
    rm -f libs/cobalt/build.jam.bak
  '';

  postInstall = ''
    ver=${boost.version}
    test -e "$out/lib/libboost_cobalt.dylib" -o -e "$out/lib/libboost_cobalt.so"
    test -d "$out/lib/cmake/boost_cobalt-$ver" -o -d "$dev/lib/cmake/boost_cobalt-$ver"
    test -d "$out/lib/cmake/boost_headers-$ver" -o -d "$dev/lib/cmake/boost_headers-$ver"
    test -e "$dev/include/boost/cobalt/spawn.hpp" -o -e "$out/include/boost/cobalt/spawn.hpp"
  '';
})
