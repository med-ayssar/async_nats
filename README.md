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

`async_nats::main` is `boost::cobalt::task<int>`. `connect`, JetStream, the key-value store, and the object store are coroutines. The client API is `<async_nats.h>`. The runtime is `<async_nats/core.h>`.

`main` calls a private event loop to set up the runtime before `co_main`. That loop runs the `io_context` on one thread. `NATS_EVENT_LOOP_WORKER_THREADS` sizes the worker pool and defaults to 1. The pool accepts up to twice `std::thread::hardware_concurrency()`, and at least 2. A larger value is clamped. `async_nats::core::instance()` is the process-wide runtime: `io_threads()` returns 1, `worker_threads()` reports the pool size, and `thread_pool()` is the pool for `boost::asio::post`.

```cpp
#include <async_nats/core.h>

boost::asio::post(async_nats::core::instance().thread_pool(), [] {
  // blocking work
});
```

`examples/pub_sub.cpp` connects, subscribes, publishes, and reads the message back. Build it with `-DBUILD_TESTS=ON`; the executable is `pub_sub`. It uses `NATS_URL`, or `nats://127.0.0.1:4222`.

```cpp
auto client = co_await async_nats::connect("nats://127.0.0.1:4222");
auto subscription = co_await client.subscribe("async_nats.example");
co_await client.publish("async_nats.example", "hello");
auto message = co_await subscription.next();
```

`connect` throws `async_nats::error` when the server cannot be reached. A later dropped connection completes `closed()` with `error_kind::interrupted`. Both are logged. `error::kind()` is `unreachable`, `interrupted`, `closed`, `signal`, or `other`.

The library handles `SIGINT` and `SIGTERM`. It closes every client, logs the signal, then runs the handler registered with `on_error`. That handler receives the `error` so the application can do extra work. `SIGKILL` cannot be handled. Call `on_error` before the first `co_await` in `co_main`.

```cpp
async_nats::on_error([](async_nats::error failure) -> boost::cobalt::task<void> {
  co_return;
});
```

`subscribe` also accepts a list of routes. Each route is a subject and a handler. A message on that subject starts the handler. `unsubscribe` takes the subjects to remove.

```cpp
co_await client.subscribe({
    {"Grok", [](async_nats::message message) -> boost::cobalt::task<void> {
       co_return;
     }},
});
co_await client.unsubscribe({"Grok"});
```

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
./build.sh            # async-nats-clang-release, plus compile_commands.json
./build.sh --gcc --debug
./install.sh          # async-nats-clang-release into the Nix profile
./install.sh --clang --debug
```

The flake packages are the four compiler and build-type combinations:

```bash
nix build .#async-nats-clang-release
nix build .#async-nats-clang-debug
nix build .#async-nats-gcc-release
nix build .#async-nats-gcc-debug
```

`default`, `clang`, and `gcc` are the Release packages. `async-nats-tests` is the Clang Release library with `-DBUILD_TESTS=ON`. `nix build .#tests` is the same derivation.

Nix installs the package and links it at `./result`:

```text
result/lib/libasync_nats.a
result/include/async_nats.h
result/include/async_nats/core.h
result/lib/cmake/async_nats/
```

`./build.sh` also configures `build/clang` or `build/gcc` and links `compile_commands.json` at the repository root. CMake writes that file because `CMAKE_EXPORT_COMPILE_COMMANDS` is on. The Nix compiler hides Boost and spdlog in its implicit include path, and CMake leaves those paths out of the database. The script writes them back as `-isystem` flags so Homebrew `clangd` can see them without running the Nix compiler. `nix build` alone keeps its database inside the sandbox.

A development shell with the matching compiler, CMake, Ninja, Boost, and spdlog:

```bash
nix develop .#async-nats-clang
nix develop .#async-nats-gcc
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
