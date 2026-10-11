# How AsyncNats runs

AsyncNats wraps the NATS C client with Boost.Cobalt tasks. The C library owns
the NATS protocol, connection management, and subscription callbacks. AsyncNats
registers its `AsyncNatsEventLoop` adapter through `natsOptions_SetEventLoop`,
so socket readiness is processed on the `Core` Asio IO context.

## Connection calls

The coroutine API stays in `client.h`; `Client.cpp` translates its operations
to `natsConnection_*`, `natsSubscription_*`, and `natsMsg_*` calls. Calls that
can block, including connection setup and request/reply, run on the worker pool
and resume on the coroutine's executor. The NATS event-loop callbacks post
socket registration, readiness, and detach work to the one IO thread.

`EventLoop::run` closes tracked connections after `co_main` completes, keeps
the IO context alive while the C client detaches its sockets, then shuts down
the NATS library and joins the IO thread.

## Subscriptions

The pull-subscription path uses a NATS asynchronous subscription callback to
queue messages for `Subscription::next()`. A parked `next()` is resumed on its
executor when a message arrives, the subscription is removed, or the connection
closes. The route path uses the same native callback and starts each
`MessageHandler` on the worker pool without waiting for it.

The callback copies the subject, payload, and optional reply subject from the
native `natsMsg`, then destroys the C message before posting the copy to the
coroutine side. JetStream helpers continue to use the public `Client` API.

## Errors and shutdown

NATS connection and asynchronous error callbacks translate native errors into
`AsyncNats::Error`, log them, and run the registered `onError` handler. Explicit
close and process shutdown fail parked waiters with the corresponding error
kind. A signal closes tracked clients before invoking `onError`.

The implementation files are `src/AsyncNats/client/Client.cpp`,
`src/AsyncNats/core/eventLoop/NatsEventLoop.cpp`, and
`src/AsyncNats/core/eventLoop/EventLoop.cpp`. The Nix package and development
shell include `libnats-c`; installed CMake consumers also find its `cnats`
package.
