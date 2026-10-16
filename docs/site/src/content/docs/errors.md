---
title: Errors and signals
description: ErrorKind, onError, and the SIGINT and SIGTERM path.
---

The generated page is [`Error`](/doxygen/class_async_nats_1_1_error.html). Every failure the library reports is an `AsyncNats::Error`. `what()` is the message. `kind()` is one of:

| What happened | Kind | What the library does |
| --- | --- | --- |
| The NATS C client cannot establish the connection | `unreachable` | Log, run `onError`, then `connect` throws |
| The NATS C client permanently loses an established connection | `interrupted` | Log, run `onError`, fail waiters, throw |
| `Client::close()` | `closed` | Resume waiters. No log and no `onError` |
| `SIGINT` or `SIGTERM` | `signal` | Unsubscribe every topic, close tracked clients, then log and run `onError` |
| A NATS API error, request timeout, or programming mistake such as a missing client | `other` | Log and report |

The messages are:

- `NATS connection failed: ...`
- `NATS connection closed`
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

`watchSignals` is already waiting before `co_main` starts. `SIGKILL` cannot be caught. A normal exit closes tracked connections while the Asio event loop is alive, then cancels the signal set and shuts down the NATS library.

On `SIGINT` or `SIGTERM` the watcher:

1. Sets the process-wide stop flag, so a `connect` still in progress throws kind `signal`.
2. Unsubscribes every topic on every tracked client, then closes the client. A parked `closed()` or `next()` resumes and throws.
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
