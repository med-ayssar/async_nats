---
title: Client
description: Connect, publish, subscribe, request, and close one NATS connection.
---

Include `<AsyncNats/client.h>`. The generated pages are [`Client`](/doxygen/class_async_nats_1_1_client.html) and [`Subscription`](/doxygen/class_async_nats_1_1_subscription.html). A default-constructed `client` is empty. `connect` returns a live one. Copies share the same connection. The NATS C client owns protocol and subscription processing; its socket readiness callbacks use AsyncNats's Asio event loop.

```cpp
auto client = co_await AsyncNats::connect("nats://127.0.0.1:4222");
```

`connect` takes a `nats://host:port` URL. The NATS C client handles the protocol and reconnects. AsyncNats attaches its socket readiness callbacks to the process-wide Asio event loop, while coroutine wrappers move blocking C API calls onto the worker pool.

## Publish and request

```cpp
co_await client.publish("subject", "payload");
auto body = co_await client.request("subject", "payload");
```

`request` subscribes a one-shot inbox, publishes with that inbox as the reply subject, and returns the reply body. The overload that takes `vector<pair<string, string>>` sends those pairs as NATS headers.

## Pull subscribe

```cpp
auto subscription = co_await client.subscribe("async_nats.example");
auto message = co_await subscription.next();
co_await subscription.unsubscribe();
```

`message` has `subject`, `payload`, and an optional `reply`. `next()` returns immediately when a message is already queued. Otherwise it suspends until a message arrives, the connection fails, or `unsubscribe()` runs. Destroying a `subscription` does not send `UNSUB`.

The per-subscription queue has no limit.

## Route subscribe

```cpp
co_await client.subscribe({
    {"Grok", [](AsyncNats::Message message) -> boost::cobalt::task<void> {
       co_return;
     }},
});
co_await client.unsubscribe({"Grok"});
```

Each route is a subject and a `MessageHandler`. The handler is `task<void>(message)`. The native NATS callback starts it on the worker pool and does not wait, so a slow handler does not stall socket processing. A thrown handler is logged and the connection stays up.

An empty route list does nothing. Duplicate subjects become separate subscriptions. `unsubscribe` sends `UNSUB` for every route on those subjects and drops the handlers. Unknown subjects are skipped.

## Close

```cpp
co_await client.close();
co_await client.closed();
```

`close()` closes the C client connection and wakes waiters with `ErrorKind::closed`. It does not log and does not run `onError`. An empty client returns immediately.

`closed()` suspends until the connection fails or `close()` runs, then throws the stored `error`. The first reason sticks. A later failure does not replace it.
