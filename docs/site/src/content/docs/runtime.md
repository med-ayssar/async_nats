---
title: How it runs
description: How AsyncNats wraps the NATS C client with coroutines and an Asio event loop.
---

AsyncNats wraps the NATS C client with Boost.Cobalt tasks. The C library owns
the NATS protocol, connection management, and subscription callbacks. The
library's `main` starts one `Core` Asio IO context. Since the C API attaches
the event loop through each connection's `natsOptions`, every `connect()` uses
the same `AsyncNatsEventLoop` callbacks and IO context with
`natsOptions_SetEventLoop`.

## Startup

`main` creates an `EventLoop`. Its `setup()` starts the `Core` singleton before
running the application's `co_main`. The runtime has one IO thread and a worker
pool sized by `NATS_EVENT_LOOP_WORKER_THREADS`.

When `co_main` finishes, the event loop closes tracked NATS connections while
the IO context is still running. This lets the C library detach its sockets
through the adapter before AsyncNats shuts down the NATS library and joins the
IO thread.

## Coroutine wrappers

`Client.cpp` wraps `natsConnection_*`, `natsSubscription_*`, and `natsMsg_*`
functions. Calls that can block, including connection setup and request/reply,
run on the worker pool and resume on the coroutine's executor.

The event-loop adapter assigns each native socket to Asio and routes read/write
readiness to `natsConnection_ProcessReadEvent` and
`natsConnection_ProcessWriteEvent`. Registration and detach callbacks are
serialized on the IO context.

## Subscriptions

Subscription forms use the native C message callback. Pull subscriptions
queue copied messages for `Subscription::next()`. Route subscriptions start
their `MessageHandler` on the worker pool. Queue members use
`natsConnection_QueueSubscribe` for the same two forms. The callback copies the
subject, payload, and optional reply subject, destroys the native `natsMsg`,
and then posts the copy to AsyncNats.

JetStream helpers continue to use the public `Client` operations and therefore
share the same native connection and event loop.
