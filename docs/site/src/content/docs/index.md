---
title: AsyncNats
description: C++23 NATS client. The library owns main and runs your co_main coroutine.
---

AsyncNats is a static library. Link `AsyncNats::AsyncNats`, include `<AsyncNats.h>`, and define `co_main`. The library supplies process `main`. `AsyncNats::Main` is `boost::cobalt::task<int>`.

```cpp
#include <AsyncNats.h>

AsyncNats::Main co_main(int argc, char* argv[]) {
  auto client = co_await AsyncNats::connect("nats://127.0.0.1:4222");
  co_await client.publish("async_nats.example", "hello");
  co_return 0;
}
```

The integer `co_main` returns is logged. The process exits 0 unless an exception escapes. Register `onError` before the first `co_await`.

The client API is `<AsyncNats.h>`. The runtime is `<AsyncNats/Core.h>`, class `Core`. Socket work stays on one IO thread. `NATS_EVENT_LOOP_WORKER_THREADS` sizes the worker pool and defaults to 1.

| Page | What it covers |
| --- | --- |
| [API](/api/) | Every function, with its signature |
| [Doxygen](/doxygen/index.html) | The generated reference for the same declarations |
| [How it runs](/runtime/) | Startup, the read loop, and how a subscription waits |
| [Core](/core/) | `Core::instance()`, the IO thread, and the worker pool |
| [Core implementation overview](/implementation/) | The runtime, signals, and `packageFor` / `shellFor` |
| [Client](/client/) | Connect, publish, subscribe, request, close |
| [Errors and signals](/errors/) | `ErrorKind`, `onError`, `SIGINT`, and `SIGTERM` |
| [JetStream](/jetstream/) | Key-value and the object store |

`examples/pub_sub.cpp` connects, subscribes, publishes, and reads one message. Build it with `-DBUILD_TESTS=ON`. It uses `NATS_URL`, or `nats://127.0.0.1:4222`.
