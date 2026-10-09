# How AsyncNats works

This note is for reading the implementation later. The same text is the Astro page [Core implementation overview](../docs/site/src/content/docs/implementation.md). The shorter public walkthrough is `docs/flow.md`. This file follows the same path further into the source, then explains `packageFor` and `shellFor` in `flake.nix` and the matching functions in the docktopus flake.

The library owns process `main`. An application defines `co_main` and links `AsyncNats::AsyncNats`. `AsyncNats::Main` is `boost::cobalt::task<int>`. The public client header is `<AsyncNats/client/AsyncNats.h>`. The runtime header is `<AsyncNats/core/Core.h>`.

## What is compiled

CMake lists sources by hand in the repository `CMakeLists.txt`.

| Target | Sources | Role |
|---|---|---|
| `AsyncNatsObjects` | `client/Client.cpp`, `client/JetStream.cpp`, `core/Core.cpp`, `core/utils/ThreadCount.cpp` | The client and the runtime, with no `main`. Tests link this. |
| `AsyncNats` | `core/Main.cpp`, `core/eventLoop/EventLoop.cpp`, plus the objects above | The static library an application links. It supplies `main`. |

The static library takes those objects with `$<TARGET_OBJECTS:AsyncNatsObjects>` and `add_dependencies`. It does not `target_link_libraries` the object library. Public include directories are `client/include` and `core/include`. Private include directories are `core/eventLoop/include` and `core/utils/include`. Headers resolve as `<AsyncNats/client/...>`, `<AsyncNats/core/...>`, and `<AsyncNats/eventloop/...>`. Installed names are `libAsyncNats.a`, `include/AsyncNats/client/AsyncNats.h`, `include/AsyncNats/core/Core.h`, and `lib/cmake/AsyncNats/`.

`Session.h` is `client/include/AsyncNats/client/Session.h`. `reportError`, `closeAllClients`, `requestStop`, and `stopRequested` are friends of `Client` and are defined in `Client.cpp`. That header is not installed. `EventLoop.h` is `core/eventLoop/include/AsyncNats/eventloop/EventLoop.h` and is not installed. `ThreadCount` only parses the worker-pool size, so it lives in `core/utils/include/AsyncNats/core`.

## Two executors

`Core::instance()` is a process-wide singleton, built on the first call from `EventLoop::setup()`. Its state is in `core/Core.cpp`.

One `std::thread` runs `io_context::run()`. A work guard keeps that `run()` from returning while the context is idle. `ioThreads()` always returns 1. `NATS_EVENT_LOOP_IO_THREADS` is not read.

A `boost::asio::thread_pool` sits beside that thread. `NATS_EVENT_LOOP_WORKER_THREADS` sizes it and defaults to 1 when the variable is missing, empty, zero, negative, or not a number. `maxThreadCount()` is twice `std::thread::hardware_concurrency()`, and a reported 0 is treated as 1, so the cap is at least 2. A larger value is logged and clamped. `workerThreads()` reports the size that was actually used. `threadPool()` is that pool.

`ioContext()`, `release()`, and `join()` are private and `friend class EventLoop`. `release()` drops the work guard so `run()` can return once the posted work finishes. `join()` joins the IO thread. The pool joins in its own destructor.

Socket operations are not thread-safe. Each connection owns a strand on the IO context. The read loop is the only reader. Writes take a `write_busy` flag so only one `async_write` is outstanding; further writers park in `write_waiters` and are posted back onto the socket executor when the current write finishes.

`enter()` is how every client method reaches that strand:

```cpp
co_await boost::asio::dispatch(socket.get_executor(), boost::cobalt::use_op);
```

A route handler runs on the worker pool. When that handler `co_await`s `publish`, `subscribe`, or `write`, `enter()` moves the coroutine onto the strand before the socket is touched. Pull `next()` waiters are resumed on the socket executor, so they never leave it.

Cobalt tasks are lazy. Calling a handler builds the coroutine frame and does not run the body. `boost::cobalt::spawn(executor, task, completion)` sets the promise executor and dispatches the coroutine onto that executor, which is where the body starts.

## Startup

`core/Main.cpp` is the whole process entry point:

```cpp
auto main(int argc, char* argv[]) -> int {
  AsyncNats::EventLoop loop;
  loop.setup();
  return loop.run(argc, argv);
}
```

`setup()` stores `&Core::instance()`. A second call returns immediately. `run()` then:

1. Builds a `signal_set` on the IO context for `SIGINT` and `SIGTERM`.
2. Spawns `watchSignals` on that context. Its completion only logs `signal handler failed` if the watcher throws.
3. Spawns `co_main(argc, argv)` on the same context. The completion logs `co_main returned N`, or `Exception ...` if `co_main` throws.
4. Cancels the signal set, calls `release()`, and fulfills a `std::promise`.
5. The thread that called `run()` blocks on that promise, then `join()`s the IO thread and returns 0.

The integer from `co_main` is only logged. The process exit code from `run()` is 0 whether `co_main` returned or threw. Register `onError` before the first `co_await` inside `co_main`, because the watcher is already waiting when `co_main` starts.

## Connect

`connect` parses `nats://host:port`. The port defaults to 4222. It checks a process-wide stop flag, then resolves, checks again, connects TCP on a new strand, checks again, reads `INFO`, writes `CONNECT` plus `PING`, waits until `PONG`, and checks the flag once more. A `PING` that arrives during the handshake is answered with `PONG`. A line that starts with `-ERR`, or an `INFO` line that does not start with `INFO `, becomes kind `interrupted`.

After the last check the connection is stored in the process registry and `readLoop` is spawned on the socket executor. `readLoop` is the only reader for the life of the socket.

Resolve or TCP failure is kind `unreachable`, logged through `reportError`, then thrown:

`NATS server at host:port is not reachable: ...`

A handshake or socket failure after the TCP connect is kind `interrupted`:

`connection to NATS server host:port was interrupted: ...`

A stop flag seen before the socket exists throws kind `signal` and does not track a client. A stop flag seen after the socket exists calls `shutdown` with kind `signal`, then throws the same error. That client was not tracked, so the signal watcher does not close it a second time.

## The read loop

`readIncoming` pulls protocol lines out of one buffer:

- `PING` is answered with `PONG`.
- A line that starts with `-ERR` throws `Error` with kind `other`.
- Anything that is neither `MSG` nor `HMSG` is skipped.
- `MSG` and `HMSG` are parsed into a subject, a subscription id, an optional reply subject, and a payload. `HMSG` header bytes are stripped at the first `\r\n\r\n`. The payload length is the last field of the line. The trailing `\r\n` after the payload is consumed and not returned.

`deliver` then looks up that sid:

1. A route handler is invoked to produce a `task<void>`. That task is spawned on `Core::instance().threadPool().get_executor()`. `deliver` does not `co_await` it, so a slow handler does not stall the read loop. A throw while building the task, or a throw from the running handler, is logged as `nats handler failed`. The connection stays up.
2. A parked `next()` or `request()` waiter is resumed on the socket executor and receives the message.
3. With neither a handler nor a waiter, the message is `push_back`ed on `queues[sid]`. That deque is unbounded. `SUB` is sent with no pending limit, so a fast publisher can grow it for the life of the connection.

When `readLoop` leaves the loop with a new failure, it calls `failWaiters` and then `reportError`. `operation_aborted` is ignored, because that is the library cancelling the socket during `shutdown`.

## Subscriptions

### Route subscribe

`subscribe(vector<Route>)` assigns a sid to each route, stores the handler, remembers the sid under the subject, and writes one `SUB` frame for the whole list. The call returns after the write. It does not wait for a message. Duplicate subjects become separate sids. An empty list returns immediately. If the write throws, the sids just added are removed from `handlers` and `routes`.

The application then waits on the connection:

```cpp
co_await client.subscribe({
    {"Grok", [client](AsyncNats::Message message) mutable -> boost::cobalt::task<void> {
       co_await client.publish("Signal", message.payload);
     }},
});
co_await client.closed();
```

The lambda is `mutable` when it calls `publish`, because `Client::publish` is non-const and a `std::function` target is const unless the lambda says otherwise.

`closed()` moves onto the strand. If the connection has already failed, it throws the stored reason and kind immediately. Otherwise it suspends and pushes its handle onto `close_waiters`. Every later message is another spawned handler until something fails the connection.

`unsubscribe(subjects)` writes `UNSUB` for each route sid of those subjects, then erases the handlers, the queues, and the route entries. Subjects that were never subscribed are skipped.

### Pull subscribe

`subscribe(subject)` writes one `SUB` and returns a move-only `Subscription`. Destroying it does not send `UNSUB`. `next()` calls `receive(sid)`:

1. A queued message is returned immediately.
2. A connection that has already failed throws. `receive` builds that `Error` from the stored reason text. The `Error` constructed at the waiter’s `await_resume` uses the default kind, `other`. `closed()` is the waiter that preserves `fail_kind`.
3. Otherwise the coroutine suspends. There is one waiter per sid. A second `next()` on the same sid replaces that map entry.

`unsubscribe()` on the subscription sends `UNSUB`, drops the queue, and if a `next()` is parked it resumes that coroutine with the reason `subscription closed`.

`request()` uses the same wait. It subscribes a one-shot inbox (`SUB`, then `PUB` or `HPUB` with that inbox as the reply subject, then `UNSUB <sid> 1`) and `co_await`s `receive` for the reply payload.

## Errors

Every failure the library reports is an `AsyncNats::Error`. `what()` is the message. `kind()` is one of:

| What happened | Kind | What the library does |
|---|---|---|
| Resolve or TCP connect fails | `unreachable` | Log, run `onError`, then `connect` throws |
| The socket dies, the peer sends EOF, or the handshake fails | `interrupted` | Log, run `onError`, fail waiters, throw |
| `Client::close()` | `closed` | Resume waiters. No log and no `onError` |
| `SIGINT` or `SIGTERM` | `signal` | Close tracked clients first, then log at info and run `onError` |
| A `-ERR` on a live connection, or a programming mistake such as an empty client | `other` | Log and report when it comes from the read loop |

`failWaiters` is sticky. The first call stores the reason and kind, marks the connection failed, and resumes every parked `next()` and every `closed()` on the socket executor. A later failure returns without replacing that reason. That is why a read-loop error after `close()` does not overwrite `connection closed`.

`reportError` logs `spdlog::error`, or `spdlog::info` for kind `signal`. Kind `closed` is not logged. It then runs the handler registered with `onError`, unless that handler is already on the stack (`sessions().in_handler`). The flag is cleared when `reportError` returns, including when the handler throws. A throw from the handler is logged as `nats error handler failed` and does not escape.

A write that fails for a reason other than `operation_aborted`, on a connection that has not already failed, becomes kind `interrupted`: waiters are failed, `onError` runs, and the write throws.

`Client::close()` calls `shutdown("connection closed", ErrorKind::closed)`. That cancels the socket, shuts it down both ways, closes it, clears handlers and routes, and runs `failWaiters`. It does not call `reportError`.

An application that wants the library log and `onError` to cover the expected endings, and still wants programming errors to surface, looks like this:

```cpp
try {
  auto client = co_await AsyncNats::connect(url);
  co_await client.subscribe({/* routes */});
  co_await client.closed();
} catch (const AsyncNats::Error& failure) {
  if (failure.kind() == AsyncNats::ErrorKind::other) {
    throw;
  }
}
co_return 0;
```

The event loop logs a rethrown `other` as `Exception ...` and still exits 0.

## Signals

`watchSignals` in `core/eventLoop/EventLoop.cpp` waits once:

```cpp
auto [status, signo] = co_await signals.async_wait(boost::asio::as_tuple(boost::cobalt::use_op));
```

`SIGKILL` cannot be caught. A normal exit of `co_main` cancels the set. The wait then completes with an error, `status` is set, and the watcher returns. It does not close clients and it does not log.

On `SIGINT` or `SIGTERM` the watcher does four steps, in this order:

1. `requestStop()` sets `sessions().stopping`. `connect` reads that flag before resolve, after resolve, after TCP connect, and after the handshake. Both the watcher and `co_main` run on the one IO thread, so they only interleave at `co_await` points.
2. `closeAllClients` locks every tracked connection, clears the registry, and `shutdown`s each socket with the signal error. `failWaiters` resumes a parked `closed()` or `next()`, which then throws.
3. The message is `received SIGINT, closing NATS client`, `received SIGTERM, closing NATS client`, or `received signal <number>, closing NATS client`.
4. `reportError` logs that message at info level and runs `onError`.

The clients are already closed before the callback runs. The callback does its extra work and returns. Waiting on `closed()` again from inside the callback throws the signal error into the handler, and `reportError` logs that as `nats error handler failed`.

```cpp
AsyncNats::onError([](AsyncNats::Error failure) -> boost::cobalt::task<void> {
  if (failure.kind() == AsyncNats::ErrorKind::signal) {
    spdlog::info("shutting down");
  }
  co_return;
});
```

`onError` is process-wide. Passing an empty handler clears it. Handlers that were already spawned when the signal arrived are not cancelled. They run to completion, or they fail on their own if they touch the closed socket. The next `publish` or `write` then takes the write-failure path, and `failWaiters` keeps the original signal reason because it is sticky.

## JetStream

`JetStream.cpp` does not open another socket. `jetstream::make(client)` returns a context that shares the `Client`. Key-value and object-store methods are Cobalt tasks. They publish and request on JetStream API subjects, then parse the JSON reply in this file. The same strand, read loop, and error kinds apply. The store method that removes a key or an object is `remove`.

## `packageFor` and `shellFor`

Both functions live in the `let` of `flake.nix`. `eachDefaultSystem` calls them once per system (`aarch64-darwin`, `aarch64-linux`, `x86_64-darwin`, `x86_64-linux`) and publishes the results as `packages` and `devShells`. Hyphenated attribute names are quoted. A name with no hyphen, such as `clang`, is a bare identifier.

### Shared setup: `pkgsFor` and `stdenvOf`

`stdenvOf prev compiler` returns `gccStdenv` when `compiler` is `"gcc"`, and `clangStdenv` otherwise.

`pkgsFor system compiler` imports nixpkgs with two overlays, in this order:

1. A compiler overlay. When the chosen stdenv differs from nixpkgs’ default stdenv, it rebuilds `boost190` and `spdlog` with that stdenv. On Darwin the default stdenv is already Clang, so the Clang path leaves both packages as nixpkgs built them. The GCC path rebuilds both with GCC.
2. `overlays.default`, from `nix/boost-with-cobalt.nix`. Nixpkgs Boost 1.90 ships Cobalt headers and does not build `libboost_cobalt` unless b2 is passed `cxxstd=23`. This overlay replaces `pkgs.boost190` with that C++23 build. It sees the package the compiler overlay produced, so a GCC package builds Cobalt with GCC.

`libraries/default.nix` is the private-dependency set. In this repository the set is empty. `packageFor` and `shellFor` both import it and pass `builtins.attrValues libraries` onward, so a dependency added there lands in the package and in the shell.

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

`pkgs.callPackage ./package.nix` fills `cmake`, `ninja`, and `lib` from that package set and receives `stdenv`, `boost`, `spdlog`, `catch2_3`, `libraries`, `withTests`, and `buildType` explicitly. `package.nix` sets `cmakeBuildType = buildType`. When `buildType` is `"Debug"` it also sets `dontStrip`, so the static archive keeps its DWARF. A Release build is stripped as usual.

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

There is no `buildType` and no `withTests`. The shell is a compiler toolchain, not a library build. `(pkgs.mkShell.override { inherit stdenv; })` makes `nix develop` use Clang or GCC. `packages` is cmake, ninja, `boost190`, spdlog, Catch2, and the private `libraries` set. Because that set is empty, the shell does not contain `libAsyncNats.a`.

`devShells.default`, `devShells.clang`, and `devShells.async-nats-clang` are `shellFor system "clang"`. `devShells.gcc` and `devShells.async-nats-gcc` are `shellFor system "gcc"`. `./build.sh` uses the shell only after the package build, to configure `build/${COMPILER}` with `compile_commands.json`. That configure is always CMake Debug, including after `--release`, so clangd sees a Debug command line. The Nix package itself follows the selected attribute.

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
