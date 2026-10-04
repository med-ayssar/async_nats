# async_nats

Static library that owns `main` and runs a user `co_main` on a Boost.Cobalt task. Link `Nats::async_nats` and define:

```cpp
#include <async_nats/main.hpp>

async_nats::main co_main(int argc, char* argv[]) {
  co_return 0;
}
```

`async_nats::main` is `boost::cobalt::task<int>`.

## Requirements

- CMake 3.20 or newer
- Ninja
- Conan 2
- A C++23 compiler. The default Conan profile uses Homebrew LLVM Clang with libc++.

Dependencies installed by Conan: Boost 1.90 (Cobalt) and spdlog 1.17.

## Build and install

From this directory:

```bash
./build.sh
```

That configures a Debug Ninja build, builds the `async_nats` target, and installs the `library` component into `./install`.

```bash
./build.sh -p /opt/async_nats   # install prefix
./build.sh -f                  # delete ./build before configuring
./build.sh -h
```

Installed layout:

```text
install/lib/libasync_nats.a
install/include/async_nats/main.hpp
install/lib/cmake/async_nats/
```

## Use the installed package

The package calls `find_dependency` for Boost (component `cobalt`) and spdlog, so those packages must be visible to CMake. Point it at this install prefix and at the Conan generators from `./build.sh`:

```bash
cmake -S your_app -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/async_nats/build/Release/generators/conan_toolchain.cmake \
  -DCMAKE_PREFIX_PATH=/path/to/async_nats/install \
  -DCMAKE_BUILD_TYPE=Debug
```

```cmake
find_package(async_nats REQUIRED)
target_link_libraries(your_app PRIVATE Nats::async_nats)
```

Building this repository on its own also configures the sample executable `async_app`. `./build.sh` does not build or install that sample. It installs only the library component.
