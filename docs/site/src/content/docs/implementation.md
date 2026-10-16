---
title: Core implementation overview
description: How the runtime, NATS C adapter, signals, and Nix package functions are implemented.
---

This page follows the implementation: the NATS C wrapper, Asio event loop, subscriptions, shutdown, and `packageFor` / `shellFor`. The same text is `internal/doc.md` in the repository. The shorter walkthrough is [How it runs](/runtime/).

The library owns process `main`. An application defines `co_main` and links `AsyncNats::AsyncNats`. `AsyncNats::Main` is `boost::cobalt::task<int>`. The entry declarations are in `<AsyncNats.h>`, the client API is in `<AsyncNats/client.h>`, and the runtime header is `<AsyncNats/core/Core.h>`.

## What is compiled

CMake lists sources explicitly. `AsyncNatsObjects` contains the coroutine client, the NATS event-loop adapter, JetStream helpers, and the runtime. `AsyncNats` adds process `main` and the signal-aware `EventLoop`. The package links `libnats-c` and exports its `cnats` CMake dependency to consumers.

## Event loop and coroutine calls

`EventLoop::setup()`, called by the library's `main`, starts the shared `Core` runtime and its Asio IO context. The NATS C API takes its event loop through each connection's `natsOptions`, so `connect()` attaches the adapter to every connection using that same context. Each connected NATS socket is assigned to Asio, which invokes `natsConnection_ProcessReadEvent` or `natsConnection_ProcessWriteEvent` when the descriptor is ready. Attach, read, write, close, and detach operations are serialized on the shared IO context.

`Client.cpp` wraps the C API. Operations that can block, such as connecting and request/reply, run on the worker pool and resume on the coroutine executor. The NATS C message callback copies the subject, payload, and optional reply from `natsMsg`, destroys the C message, and posts the copy to AsyncNats.

## Subscriptions and shutdown

Pull subscriptions queue messages for `Subscription::next()`. Route subscriptions start their coroutine handler on the worker pool. Queue members use `natsConnection_QueueSubscribe` for either form: one pull subscription, or one or more handlers. Explicit unsubscribe and connection shutdown wake parked waiters with the appropriate `ErrorKind`.

When `co_main` completes, `EventLoop::run` closes tracked connections while the Asio loop is still running. That gives the C adapter time to release socket handles and call `natsConnection_ProcessDetachedEvent`. AsyncNats then calls `nats_Close` and joins the IO thread.

## `packageFor` and `shellFor`

Both functions live in the `let` of `flake.nix`. `eachDefaultSystem` calls them once per system (`aarch64-darwin`, `aarch64-linux`, `x86_64-darwin`, `x86_64-linux`) and publishes the results as `packages` and `devShells`. Hyphenated attribute names are quoted. A name with no hyphen, such as `clang`, is a bare identifier.

### Shared setup: `pkgsFor` and `stdenvOf`

`stdenvOf prev compiler` returns `gccStdenv` when `compiler` is `"gcc"`, and `clangStdenv` otherwise.

`pkgsFor system compiler` imports nixpkgs with two overlays, in this order:

1. A compiler overlay. When the chosen stdenv differs from nixpkgs’ default stdenv, it rebuilds `boost190` and `spdlog` with that stdenv. On Darwin the default stdenv is already Clang, so the Clang path leaves both packages as nixpkgs built them. The GCC path rebuilds both with GCC.
2. `overlays.default`, from `nix/boost-with-cobalt.nix`. Nixpkgs Boost 1.90 ships Cobalt headers and does not build `libboost_cobalt` unless b2 is passed `cxxstd=23`. This overlay replaces `pkgs.boost190` with that C++23 build. It sees the package the compiler overlay produced, so a GCC package builds Cobalt with GCC.

`libraries/default.nix` is the optional private-dependency set. It is empty in this repository. `libnats-c` is a direct nixpkgs dependency passed to `packageFor` and `shellFor`.

### `packageFor` in this repository

```nix
packageFor = system: compiler: buildType: withTests: ...
```

The four arguments select one derivation:

| Argument | Values | Effect |
|---|---|---|
| `system` | a default Nix system | Which nixpkgs instance is imported. |
| `compiler` | `"clang"` or `"gcc"` | `stdenv` passed into `package.nix`, and which overlay path `pkgsFor` takes. |
| `buildType` | `"debug"` or `"release"` | Mapped to the CMake string `"Debug"` or `"Release"`. Any other string becomes `"Release"`. |
| `withTests` | `true` or `false` | Adds Catch2, passes `-DBUILD_TESTS=ON`, and runs `ctest` in `checkPhase`. |

`pkgs.callPackage ./package.nix` fills `cmake`, `ninja`, and `lib` from that package set and receives `stdenv`, `boost`, `spdlog`, `libnats-c`, `catch2_3`, `libraries`, `withTests`, and `buildType` explicitly. `package.nix` sets `cmakeBuildType = buildType`. When `buildType` is `"Debug"` it also sets `dontStrip`, so the static archive keeps its DWARF. A Release build is stripped as usual.

`CMakeLists.txt` sets `CMAKE_MAP_IMPORTED_CONFIG_DEBUG` to `Release RelWithDebInfo ""`. A Debug build of this library can therefore link the Release Boost and spdlog that Nix installed. That map belongs to the library. An application that wants the Debug archive uses its own map.

The published attributes are:

| Attribute | Call |
|---|---|
| `packages.default` | `packageFor system "clang" "debug" false` |
| `async-nats-clang-debug` | the same derivation as `default` |
| `async-nats-clang-release`, also `clang` | `packageFor system "clang" "release" false` |
| `async-nats-gcc-release`, also `gcc` | `packageFor system "gcc" "release" false` |
| `async-nats-gcc-debug` | `packageFor system "gcc" "debug" false` |
| `async-nats-tests`, also `tests` | `packageFor system "clang" "release" true` |

`nix build` with no attribute is Clang Debug. `.#clang` and `.#gcc` stay Release. `./build.sh` and `./install.sh` default to `async-nats-clang-debug`. `--gcc` keeps Debug. `--release` selects the Release attribute for the chosen compiler.

Nix copies the Git tree of this flake into the build. A dirty tracked file is included. An untracked source is omitted until `git add`. The dirty-tree warning is harmless. `package.nix` also drops `build`, `install`, `result`, `.cache`, and `.git` from the source it hands to CMake.

### `shellFor` in this repository

```nix
shellFor = system: compiler: ...
```

There is no `buildType` and no `withTests`. The shell is a compiler toolchain, not a library build. `(pkgs.mkShell.override { inherit stdenv; })` makes `nix develop` use Clang or GCC. Its hook sets `CXX` to that compiler and sources `env/main.sh`; `packages` includes cmake, ninja, clang-tools, `boost190`, spdlog, Catch2, `libnats-c`, and the private `libraries` set. The shell does not contain `libAsyncNats.a`.

`devShells.default`, `devShells.clang`, and `devShells.async-nats-clang` are `shellFor system "clang"`. `devShells.gcc` and `devShells.async-nats-gcc` are `shellFor system "gcc"`. Each shell runs `env/main.sh` from its `shellHook`, exporting the compiler and clangd. Enter the matching shell before running `./build.sh`; the script invokes CMake directly, configures the selected compiler and build type, and installs to `./install`.

`devShells.doc` is separate. It is a Node.js shell plus the `doc` command from `nix/doc.nix`, used by `./doc.sh`.

### The same names in docktopus

`/Users/ayssar.mb/Desktop/dev/projects/docktopus/flake.nix` defines the two functions again, with a different third argument and no test flag:

```nix
packageFor = system: compiler: buildType: ...
shellFor = system: compiler: buildType: ...
```

`buildType` is still `"debug"` or `"release"`, mapped to `"Debug"` or `"Release"` for the app’s own `cmakeBuildType`. Docktopus’s `package.nix` sets `dontStrip` when that value is `"Debug"`, so the app binary keeps its symbols too.

The important difference is `libraries/default.nix`. Docktopus passes `buildType` into it, and the set is one package:

```nix
async_nats = inputs.async_nats.packages.${system}."async-nats-${compiler}-${buildType}";
```

`packageFor` puts that package in `buildInputs`, so the app links the Debug archive in a Debug build and the Release archive in a Release build. `shellFor` puts the same package on the shell prefix, so `find_package(AsyncNats)` inside `nix develop` finds the archive that matches the shell. A Debug docktopus shell therefore contains `async-nats-clang-debug` or `async-nats-gcc-debug`. A Release shell contains the Release package. The compiler binary is the same in both.

Docktopus’s `CMakeLists.txt` maps imported Debug to `Debug Release RelWithDebInfo ""`. A Debug configure prefers a Debug AsyncNats package and still links a Release package when that is the one on the prefix.

| Attribute | Library it links | App `cmakeBuildType` |
|---|---|---|
| `packages.default`, `docktopus-clang-debug` | `async-nats-clang-debug` | Debug |
| `docktopus-gcc-debug` | `async-nats-gcc-debug` | Debug |
| `docktopus-clang` | `async-nats-clang-release` | Release |
| `docktopus-gcc` | `async-nats-gcc-release` | Release |

`devShells.default` is `shellFor system "clang" "debug"`. `./build.sh` builds `.#docktopus-${compiler}-debug` unless `--release` is passed, then configures the checkout with `nix develop` on that same attribute. `./start.sh` is `nix shell .`, which runs the default Clang Debug package. `source env/main.zsh` is `nix develop .`, so clangd’s compiler and the headers on the prefix come from the Clang Debug shell.

`flake.lock` pins the async_nats commit. `./build.sh` and `./start.sh` use that pin. `./install.sh` adds `--override-input async_nats git+file:../async_nats`, so an install builds the sibling checkout. An override does not rewrite the lock.
