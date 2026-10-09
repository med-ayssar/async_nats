---
title: Errors and signals
description: ErrorKind, onError, and the SIGINT and SIGTERM path.
---

The generated page is [`Error`](/doxygen/class_async_nats_1_1_error.html). Every failure the library reports is an `AsyncNats::Error`. `what()` is the message. `kind()` is one of:

| What happened | Kind | What the library does |
| --- | --- | --- |
| Resolve or TCP connect fails | `unreachable` | Log, run `onError`, then `connect` throws |
| The socket dies, the peer sends EOF, or the handshake fails | `interrupted` | Log, run `onError`, fail waiters, throw |
| `Client::close()` | `closed` | Resume waiters. No log and no `onError` |
| `SIGINT` or `SIGTERM` | `signal` | Close tracked clients first, then log and run `onError` |
| A `-ERR` on a live connection, or a programming mistake such as a missing client | `other` | Log and report when it comes from the read loop |

The messages are:

- `NATS server at host:port is not reachable: ...`
- `connection to NATS server host:port was interrupted: ...`
- `received SIGINT, closing NATS client` or `received SIGTERM, closing NATS client`

## onError

```cpp
AsyncNats::onError([](AsyncNats::Error failure) -> boost::cobalt::task<void> {
  if (failure.kind() == AsyncNats::ErrorKind::signal) {
    spdlog::info("shutting down");
  }
  co_return;
});
```

Call `onError` before the first `co_await` in `co_main`. Kind `closed` does not run the handler. A handler that is already on the stack is not entered again. An empty handler clears the previous one. A handler that throws is logged as `nats error handler failed`.

## Signals

`watchSignals` is already waiting before `co_main` starts. `SIGKILL` cannot be caught. A normal exit cancels the signal set; the wait completes with `operation_aborted`, and the watcher returns without closing clients.

On `SIGINT` or `SIGTERM` the watcher:

1. Sets the process-wide stop flag, so a `connect` still in progress throws kind `signal`.
2. Closes every tracked client. A parked `closed()` or `next()` resumes and throws.
3. Logs the signal.
4. Runs `onError`.

The clients are already closed before the callback runs. Waiting on `closed()` again from inside the callback throws the signal error into the handler.

Handlers that were already spawned when the signal arrived are not cancelled.

An application that wants programming errors to surface, and treats the other kinds as already reported, looks like this:

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
