# async_nats

Static library that owns `main` and runs a user `co_main` on a Boost.Cobalt task. Link `Nats::async_nats` and define:

```cpp
#include <async_nats/main.hpp>

async_nats::main co_main(int argc, char* argv[]) {
  co_return 0;
}
```

`async_nats::main` is `boost::cobalt::task<int>`.

The build uses Nix. C++ standard is 23. Choose Clang or GCC.

Public dependencies come from nixpkgs: Boost 1.90 and spdlog. Nixpkgs does not ship Cobalt, so `overlays.default` builds that library with C++23 and replaces `pkgs.boost190`.

Private dependencies go in `libraries/default.nix`. Pin each one in `flake.nix` by branch (`?ref=`) or commit (`?rev=`), then add it to the set. The set is empty until you declare one:

```nix
# libraries/default.nix
my_lib = inputs.my_lib.packages.${system}.${compiler};
```

## Requirements

- Nix 2.35 or newer, with flakes available. `./build.sh` passes the flakes feature itself.

## Build and install

From this directory:

```bash
./build.sh            # Clang, C++23
./build.sh --clang
./build.sh --gcc
```

Nix installs the package and links it at `./result`:

```text
result/lib/libasync_nats.a
result/include/async_nats/main.hpp
result/lib/cmake/async_nats/
```

The same packages are `nix build .#clang` and `nix build .#gcc`.

A development shell with the matching compiler, CMake, Ninja, Boost, and spdlog:

```bash
nix develop .#clang
nix develop .#gcc
```

Inside the shell, `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug` uses C++23 because the project sets `CMAKE_CXX_STANDARD` to 23.

## Use the installed package

```bash
cmake -S your_app -B build -G Ninja \
  -DCMAKE_PREFIX_PATH=/path/to/async_nats/result \
  -DCMAKE_BUILD_TYPE=Release
```

```cmake
find_package(async_nats REQUIRED)
target_link_libraries(your_app PRIVATE Nats::async_nats)
```

Boost and spdlog must be on the same prefix path. `nix develop .#clang` provides them. The package config calls `find_dependency` for both. The installed library is static, and your program supplies `co_main`. The library supplies `main`.
