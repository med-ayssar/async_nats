# How async_nats runs

The library owns process startup. An application supplies `co_main`. One IO thread reads and writes each NATS socket. A subscription either runs a handler for every message, or a caller waits on `next()` until one message is ready. Connection failures and `SIGINT` / `SIGTERM` both end that wait by resuming it with an `async_nats::error`.

The pieces live in:

- `src/async_nats/main.cpp` — process `main`
- `src/async_nats/event_loop.cpp` — runtime startup and signals
- `src/async_nats/client.cpp` — connect, read loop, subscribe, wait, close
- `src/async_nats/session.hpp` — private helpers used by the event loop
- `src/async_nats/include/async_nats.h` — the public declarations

## Startup

`main` builds an `event_loop`, calls `setup()`, then `run()`:

```cpp
auto main(int argc, char* argv[]) -> int {
  async_nats::event_loop loop;
  loop.setup();
  return loop.run(argc, argv);
}
```

`setup()` starts the `core` singleton:

- one thread running `io_context::run()`
- a `boost::asio::thread_pool` sized by `NATS_EVENT_LOOP_WORKER_THREADS` (default 1)

Socket work stays on that one IO thread, on a strand, with one outstanding read and one outstanding write. `io_threads()` returns 1. `worker_threads()` reports the pool size. `thread_pool()` is the pool for blocking work posted with `boost::asio::post`.

`run()` does three things on that `io_context`:

1. Arm a `signal_set` for `SIGINT` and `SIGTERM`, and spawn `watch_signals`.
2. Spawn the user's `co_main`.
3. When `co_main` finishes, cancel the signal set, stop the IO thread, join it, and return 0.

The integer `co_main` returns is only logged (`co_main returned N`). The process exit code is 0 unless an exception escapes `co_main`, in which case the event loop logs `Exception ...` and still returns 0.

`async_nats::main` is `boost::cobalt::task<int>`. Register `on_error` before the first `co_await` inside `co_main`.

## Connect

`connect` resolves the host, opens TCP, reads the server `INFO`, sends `CONNECT` and `PING`, and waits for `PONG`. After that handshake it records the connection and spawns `read_loop` on the socket's strand. `read_loop` is the only reader for the life of the socket.

`read_loop` waits for the next protocol line:

- `PING` is answered with `PONG`
- `MSG` and `HMSG` are parsed and passed to `deliver`
- a line that starts with `-ERR` becomes an `async_nats::error` of kind `other`
- any other line is skipped

`connect` also checks a process-wide stop flag before resolve, after resolve, after TCP connect, and after the handshake. A signal that arrives during connect sets that flag. `connect` then closes the new socket and throws kind `signal` instead of tracking another client.

## How a subscription waits

There are two subscribe paths.

### Route subscribe

`subscribe` with a list of `{subject, handler}` pairs assigns a subscription id (`sid`) to each route, stores the handler under that id, and writes one `SUB` frame per route. This call does not wait for a message. If the write fails, the routes just added are dropped and the error propagates.

When `deliver` finds a handler for that sid, it starts the handler with `boost::cobalt::spawn` and returns. It does not `co_await` the handler, so a slow handler does not stall the read loop. If the handler throws, the exception is logged as `nats handler failed` and the connection stays up.

The application then waits on the connection:

```cpp
co_await client.subscribe({
    {"Grok", [](async_nats::message message) -> boost::cobalt::task<void> {
       spdlog::info("received {} on {}", message.payload, message.subject);
       co_return;
     }},
});
co_await client.closed();
```

`closed()` moves onto the socket strand. If the connection has already failed, it throws the stored error immediately. Otherwise it suspends and stores its coroutine handle in `close_waiters`. It stays there until something fails the connection. Every message until then is a separate spawned handler.

Docktopus uses this path: one `Grok` route, then `closed()`.

### Pull subscribe

`subscribe(subject)` sends one `SUB` and returns a `subscription`. `next()` calls `receive(sid)`:

1. If that sid's queue already holds a message, `next()` returns the front message immediately.
2. If the connection has already failed, `next()` throws the stored error and kind.
3. Otherwise the coroutine suspends. Its handle is the single waiter for that sid.

A later `deliver` with no route handler resumes that waiter and gives it the message. If nobody is waiting, the message is pushed onto a per-sid queue. That queue is unbounded, and `SUB` is sent with no pending limit, so a fast publisher can grow it for as long as the connection lives.

`request()` uses the same wait. It subscribes a one-shot inbox (`UNSUB <sid> 1`), publishes with that inbox as the reply subject, and `co_await`s `receive` for the reply.

`unsubscribe` on a `subscription` sends `UNSUB`, drops that sid's queue, and if a `next()` is parked it resumes that coroutine with "subscription closed". `unsubscribe` on a list of subjects does the same for every route sid of those subjects and erases the handlers.

## How errors are handled

Every failure the library reports is an `async_nats::error`. `kind()` is one of:

| What happened | Kind | What the library does |
|---|---|---|
| Resolve or TCP connect fails | `unreachable` | Log, run `on_error`, then `connect` throws |
| The socket dies, the peer sends EOF, or the handshake gets `-ERR` or no `INFO` | `interrupted` | Log, run `on_error`, fail waiters, throw |
| `client::close()` | `closed` | Resume waiters. No log and no `on_error` |
| `SIGINT` or `SIGTERM` | `signal` | Close tracked clients first, then log and run `on_error` |
| A `-ERR` on a live connection, or a programming mistake such as a missing client | `other` | Log and report when it comes from the read loop. An application can rethrow this one |

The messages are:

- `NATS server at host:port is not reachable: ...`
- `connection to NATS server host:port was interrupted: ...`
- `received SIGINT, closing NATS client` or `received SIGTERM, closing NATS client`

The shared wake-up is `fail_waiters`. The first call sticks: it stores the reason and kind, marks the connection failed, and resumes every parked `next()` and every `closed()`. A later failure does not replace that reason. `operation_aborted` is ignored, because that is the library cancelling the socket itself during shutdown.

`report_error` logs first (`spdlog::error`, or `spdlog::info` for a signal). Kind `closed` is not logged. It then runs the handler registered with `on_error`, unless that handler is already on the stack. A handler that throws is logged as `nats error handler failed` and does not escape.

A write that fails for a reason other than `operation_aborted` becomes kind `interrupted`: waiters are failed, `on_error` runs, and the write throws. The read loop does the same for EOF, reset, and any other exception that is not an abort.

`client::close()` calls `shutdown("connection closed", error_kind::closed)`. That cancels the socket, shuts it down, closes it, clears handlers and routes, and runs `fail_waiters`. It does not call `report_error`. A waiting `closed()` or `next()` then throws kind `closed`.

An application that wants the library log to be enough, and still wants programming errors to surface, looks like this:

```cpp
try {
  auto client = co_await async_nats::connect(url);
  co_await client.subscribe({/* routes */});
  co_await client.closed();
} catch (const async_nats::error& failure) {
  if (failure.kind() == async_nats::error_kind::other) {
    throw;
  }
}
co_return 0;
```

Unreachable, interrupted, closed, and signal are already visible through the log and `on_error`, so `co_main` can return 0. Kind `other` is rethrown, and the event loop logs it.

## How signals are caught

`watch_signals` is already waiting before `co_main` starts. It waits once on the `signal_set`. `SIGKILL` cannot be caught. A normal exit cancels the set; the wait completes with `operation_aborted`, and the watcher returns without closing clients or logging.

On `SIGINT` or `SIGTERM` the watcher:

1. Sets the process-wide stop flag, so a `connect` still in progress throws kind `signal` instead of tracking a new socket.
2. Closes every tracked client. That cancels the socket, shuts it down, and runs `fail_waiters` with kind `signal`. A parked `closed()` or `next()` resumes and throws.
3. Logs `received SIGINT, closing NATS client` or the `SIGTERM` line.
4. Runs `on_error`.

The clients are already closed before the callback runs. The callback does its extra work and returns. Waiting on `closed()` again from inside the callback throws the signal error into the handler, and `report_error` logs that as `nats error handler failed`.

```cpp
async_nats::on_error([](async_nats::error failure) -> boost::cobalt::task<void> {
  if (failure.kind() == async_nats::error_kind::signal) {
    spdlog::info("shutting down");
  }
  co_return;
});
```

Handlers that were already spawned when the signal arrived are not cancelled. They run to completion or fail on their own if they touch the closed socket.
