# Nixpkgs Boost does not build Cobalt: its Jamfile requires cxx20_hdr_concepts
# and the nixpkgs b2 invocation never passes cxxstd. Build Cobalt, and the
# libraries its CMake package depends on, with C++23.
#
# overlays.default applies this to pkgs.boost190. docktopus uses that
# overlay, so both flakes produce the same Boost store path.
#
# src/main.cpp does not define main. The inline main in
# <boost/cobalt/detail/main.hpp> is emitted only for a co_main that returns
# boost::cobalt::main. AsyncNats defines main and uses task<int> co_main.
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

  postInstall = ''
    ver=${boost.version}
    test -e "$out/lib/libboost_cobalt.dylib" -o -e "$out/lib/libboost_cobalt.so"
    test -d "$out/lib/cmake/boost_cobalt-$ver" -o -d "$dev/lib/cmake/boost_cobalt-$ver"
    test -d "$out/lib/cmake/boost_headers-$ver" -o -d "$dev/lib/cmake/boost_headers-$ver"
    test -e "$dev/include/boost/cobalt/spawn.hpp" -o -e "$out/include/boost/cobalt/spawn.hpp"
  '';
})
