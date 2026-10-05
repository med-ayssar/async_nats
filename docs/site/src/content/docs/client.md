---
title: Client
description: Connect, publish, subscribe, request, and close one NATS connection.
---

Include `<AsyncNats.h>`. A default-constructed `client` is empty. `connect` returns a live one. Copies share the same connection. Socket operations are not thread-safe: one outstanding read and one outstanding write, on a strand owned by the IO thread.

```cpp
auto client = co_await AsyncNats::connect("nats://127.0.0.1:4222");
```

`connect` takes a `nats://host:port` URL. The port defaults to 4222. It resolves the host, connects, reads `INFO`, and sends `CONNECT` and `PING`. After `PONG` the read loop owns the socket.

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

Each route is a subject and a `MessageHandler`. The handler is `task<void>(message)`. The read loop starts it and does not wait, so a slow handler does not stall the socket. A thrown handler is logged and the connection stays up.

An empty route list does nothing. Duplicate subjects become separate subscriptions. `unsubscribe` sends `UNSUB` for every route on those subjects and drops the handlers. Unknown subjects are skipped.

## Close

```cpp
co_await client.close();
co_await client.closed();
```

`close()` shuts the socket down and wakes waiters with `ErrorKind::closed`. It does not log and does not run `onError`. An empty client returns immediately.

`closed()` suspends until the connection fails or `close()` runs, then throws the stored `error`. The first reason sticks. A later failure does not replace it.
