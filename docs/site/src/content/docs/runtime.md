---
title: How it runs
description: One IO thread, the worker pool, and the two ways a subscription waits.
---

The library owns process startup. An application supplies `coMain`. One IO thread reads and writes each NATS socket. A subscription either runs a handler for every message, or a caller waits on `next()` until one message is ready. Connection failures and `SIGINT` / `SIGTERM` both end that wait by resuming it with an `AsyncNats::Error`.

The same walkthrough is in `docs/flow.md` in the repository.

## Startup

`main` builds an `EventLoop`, calls `setup()`, then `run()`:

```cpp
auto main(int argc, char* argv[]) -> int {
  AsyncNats::EventLoop loop;
  loop.setup();
  return loop.run(argc, argv);
}
```

`setup()` starts the `Core` singleton:

- one thread running `io_context::run()`
- a `boost::asio::thread_pool` sized by `NATS_EVENT_LOOP_WORKER_THREADS` (default 1)

Socket work stays on that one IO thread, on a strand, with one outstanding read and one outstanding write. `ioThreads()` returns 1. `workerThreads()` reports the pool size. `threadPool()` is the pool for blocking work posted with `boost::asio::post`.

`run()` does three things on that `io_context`:

1. Arm a `signal_set` for `SIGINT` and `SIGTERM`, and spawn `watchSignals`.
2. Spawn the user's `coMain`.
3. When `coMain` finishes, cancel the signal set, stop the IO thread, join it, and return 0.

The source files are:

- `src/async_nats/core/main.cpp` — process `main`
- `src/async_nats/core/event_loop.cpp` — runtime startup and signals
- `src/async_nats/core/core.cpp` — the `Core` singleton
- `src/async_nats/client/client.cpp` — connect, read loop, subscribe, wait, close
- `src/async_nats/client/jetstream.cpp` — JetStream, key-value, and the object store

## Connect

`connect` resolves the host, opens TCP, reads the server `INFO`, sends `CONNECT` and `PING`, and waits for `PONG`. After that handshake it records the connection and spawns `readLoop` on the socket's strand. `readLoop` is the only reader for the life of the socket.

`readLoop` waits for the next protocol line:

- `PING` is answered with `PONG`
- `MSG` and `HMSG` are parsed and passed to `deliver`
- a line that starts with `-ERR` becomes an `AsyncNats::Error` of kind `other`
- any other line is skipped

`connect` also checks a process-wide stop flag before resolve, after resolve, after TCP connect, and after the handshake. A signal that arrives during connect sets that flag. `connect` then closes the new socket and throws kind `signal`.

## Route subscribe

`subscribe` with a list of `{subject, handler}` pairs assigns a subscription id to each route, stores the handler, and writes one `SUB` frame per route. This call does not wait for a message.

When `deliver` finds a handler for that id, it starts the handler with `boost::cobalt::spawn` on the worker pool and returns. It does not `co_await` the handler, so a slow handler does not stall the read loop. Client calls inside the handler move back to the socket strand before they touch the socket. If the handler throws, the exception is logged as `nats handler failed` and the connection stays up.

The application then waits on the connection with `co_await client.closed()`. Every message until then is a separate spawned handler.

## Pull subscribe

`subscribe(subject)` sends one `SUB` and returns a `subscription`. `next()` calls `receive`:

1. If that subscription's queue already holds a message, `next()` returns it immediately.
2. If the connection has already failed, `next()` throws the stored error.
3. Otherwise the coroutine suspends. Its handle is the single waiter for that subscription.

A later `deliver` with no route handler resumes that waiter and gives it the message. If nobody is waiting, the message is pushed onto a per-subscription queue. That queue is unbounded, and `SUB` is sent with no pending limit, so a fast publisher can grow it for as long as the connection lives.

`request()` uses the same wait. It subscribes a one-shot inbox (`UNSUB <sid> 1`), publishes with that inbox as the reply subject, and waits for the reply.
