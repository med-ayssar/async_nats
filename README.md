# AsyncNats

Static library that owns `main` and runs a user `coMain` on a Boost.Cobalt task. Link `AsyncNats::AsyncNats` and define:

```cpp
#include <AsyncNats.h>

AsyncNats::Main coMain(int argc, char* argv[]) {
  auto client = co_await AsyncNats::connect("nats://127.0.0.1:4222");
  auto js = co_await AsyncNats::jetstream::make(client);

  auto kv = co_await js.createKeyValue({.bucket = "store", .history = 10});
  co_await kv.put("key", "value");
  auto value = co_await kv.get("key");

  auto objects = co_await js.createObjectStore({.bucket = "files"});
  co_await objects.put("file", "hello");
  auto data = co_await objects.get("file");

  co_return 0;
}
```

`AsyncNats::Main` is `boost::cobalt::task<int>`. `connect`, JetStream, the key-value store, and the object store are coroutines. The client API is `<AsyncNats.h>`. The runtime is `<AsyncNats/Core.h>`.

`main` calls a private event loop to set up the runtime before `coMain`. That loop runs the `io_context` on one thread. `NATS_EVENT_LOOP_WORKER_THREADS` sizes the worker pool and defaults to 1. The pool accepts up to twice `std::thread::hardware_concurrency()`, and at least 2. A larger value is clamped. `AsyncNats::Core::instance()` is the process-wide runtime: `ioThreads()` returns 1, `workerThreads()` reports the pool size, and `threadPool()` is the pool for `boost::asio::post`.

```cpp
#include <AsyncNats/Core.h>

boost::asio::post(AsyncNats::Core::instance().threadPool(), [] {
  // blocking work
});
```

`examples/pub_sub.cpp` connects, subscribes, publishes, and reads the message back. Build it with `-DBUILD_TESTS=ON`; the executable is `pub_sub`. It uses `NATS_URL`, or `nats://127.0.0.1:4222`.

```cpp
auto client = co_await AsyncNats::connect("nats://127.0.0.1:4222");
auto subscription = co_await client.subscribe("async_nats.example");
co_await client.publish("async_nats.example", "hello");
auto message = co_await subscription.next();
```

`connect` throws `AsyncNats::Error` when the server cannot be reached. A later dropped connection completes `closed()` with `ErrorKind::interrupted`. Both are logged. `Error::kind()` is `unreachable`, `interrupted`, `closed`, `signal`, or `other`.

The library handles `SIGINT` and `SIGTERM`. It closes every client, logs the signal, then runs the handler registered with `onError`. That handler receives the `Error` so the application can do extra work. `SIGKILL` cannot be handled. Call `onError` before the first `co_await` in `coMain`.

```cpp
AsyncNats::onError([](AsyncNats::Error failure) -> boost::cobalt::task<void> {
  co_return;
});
```

`subscribe` also accepts a list of routes. Each route is a subject and a handler. A message on that subject starts the handler on the worker pool. `unsubscribe` takes the subjects to remove.

```cpp
co_await client.subscribe({
    {"Grok", [](AsyncNats::Message message) -> boost::cobalt::task<void> {
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
./build.sh            # Clang Debug, plus compile_commands.json
./build.sh --gcc
./build.sh --release  # Clang Release
./install.sh          # Clang Debug into the Nix profile
./install.sh --gcc --release
```

The flake packages are the four compiler and build-type combinations:

```bash
nix build .#async-nats-clang-release
nix build .#async-nats-clang-debug
nix build .#async-nats-gcc-release
nix build .#async-nats-gcc-debug
```

`nix build` with no attribute is Clang Debug (`async-nats-clang-debug`). `clang` and `gcc` stay the Release packages. `async-nats-tests` is the Clang Release library with `-DBUILD_TESTS=ON`. `nix build .#tests` is the same derivation.

Another flake includes one of those packages. Release is `async-nats-${compiler}-release`, also published as `clang` or `gcc`. Debug is `async-nats-${compiler}-debug`, and that archive keeps its debug symbols:

```nix
async_nats = inputs.async_nats.packages.${system}."async-nats-${compiler}-debug";
```

Nix installs the package and links it at `./result`:

```text
result/lib/libAsyncNats.a
result/include/AsyncNats.h
result/include/AsyncNats/Core.h
result/lib/cmake/AsyncNats/
```

`./build.sh` also configures `build/clang` or `build/gcc` and links `compile_commands.json` at the repository root. CMake writes that file because `CMAKE_EXPORT_COMPILE_COMMANDS` is on. The Nix compiler hides Boost and spdlog in its own search path, and CMake leaves those paths out of the database. `source env/main.zsh` sets `CLANGXX` to that Nix `clang++` and exports `NIX_CFLAGS_COMPILE` from the same shell. Neovim keeps Homebrew `clangd` as the language server. When `CLANGXX` is set, it passes that compiler as `--query-driver`, so clangd runs it and the compiler reports Boost and spdlog. Start Neovim from the shell where you sourced the file. `nix build` alone keeps its database inside the sandbox.

A development shell with the matching compiler, CMake, Ninja, Boost, and spdlog:

```bash
nix develop .#async-nats-clang
nix develop .#async-nats-gcc
```

Inside the shell, `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug` uses C++23 because the project sets `CMAKE_CXX_STANDARD` to 23.

## Documentation

The API site is Astro Starlight in `docs/site`. The flake derivation `.#doc` provides Node.js and the `doc` command.

```bash
./doc.sh         # nix shell .#doc, build the site, serve http://127.0.0.1:4321/
./doc.sh build   # write docs/site/dist and exit
```

## Use the installed package

```bash
cmake -S your_app -B build -G Ninja \
  -DCMAKE_PREFIX_PATH=/path/to/async_nats/result \
  -DCMAKE_BUILD_TYPE=Release
```

```cmake
find_package(AsyncNats REQUIRED)
target_link_libraries(your_app PRIVATE AsyncNats::AsyncNats)
```

Boost and spdlog must be on the same prefix path. `nix develop .#clang` provides them. The package config calls `find_dependency` for both. The installed library is static, and your program supplies `coMain`. The library supplies `main`.
