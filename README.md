# async_nats

Static library that owns `main` and runs a user `co_main` on a Boost.Cobalt task. Link `Nats::async_nats` and define:

```cpp
#include <async_nats.h>

async_nats::main co_main(int argc, char* argv[]) {
  auto client = co_await async_nats::connect("nats://127.0.0.1:4222");
  auto js = co_await async_nats::jetstream::make(client);

  auto kv = co_await js.create_key_value({.bucket = "store", .history = 10});
  co_await kv.put("key", "value");
  auto value = co_await kv.get("key");

  auto objects = co_await js.create_object_store({.bucket = "files"});
  co_await objects.put("file", "hello");
  auto data = co_await objects.get("file");

  co_return 0;
}
```

`async_nats::main` is `boost::cobalt::task<int>`. `connect`, JetStream, the key-value store, and the object store are coroutines. Include `async_nats.h` only.

The build uses Nix. C++ standard is 23. Choose Clang or GCC.

Public dependencies come from nixpkgs: Boost 1.90 and spdlog. Nixpkgs does not ship Cobalt, so `overlays.default` builds that library with C++23 and replaces `pkgs.boost190`.

Private dependencies go in `libraries/default.nix`. Pin each one in `flake.nix` by branch (`?ref=`) or commit (`?rev=`), then add it to the set. The set is empty until you declare one:

```nix
# libraries/default.nix
my_lib = inputs.my_lib.packages.${system}.${compiler};
```

## Requirements

- Nix 2.35 or newer. Flakes are enabled in `~/.config/nix/nix.conf`.

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
result/include/async_nats.h
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
