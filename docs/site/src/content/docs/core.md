---
title: Core
description: The process-wide IO thread and worker pool.
---

Include `<AsyncNats/Core.h>`. `Core` is a process-wide singleton. `main` starts it before `coMain` runs.

```cpp
#include <AsyncNats/Core.h>

boost::asio::post(AsyncNats::Core::instance().threadPool(), [] {
  // blocking work
});
```

Socket operations are not thread-safe, and they are not exposed on `Core`. Post blocking work onto `threadPool()`. NATS reads and writes stay on the single IO thread.

## Members

| Member | Meaning |
| --- | --- |
| `Core::instance()` | The process-wide runtime. Constructed on the first call. |
| `ioThreads()` | Always 1. Socket IO runs on that thread. |
| `workerThreads()` | Number of threads in the worker pool. |
| `threadPool()` | `boost::asio::thread_pool` for `boost::asio::post`. |

`NATS_EVENT_LOOP_WORKER_THREADS` sizes the pool and defaults to 1. Empty, zero, negative, and non-numeric values become 1. The pool accepts at most twice the hardware concurrency, and at least 2. A larger value is clamped.

`ioContext()`, `release()`, and `join()` are private. `EventLoop` is the only friend. The header is not a place to run the socket.

The implementation is `src/async_nats/core/core.cpp`. The pool size parser is `src/async_nats/utils/thread_count.hpp`. [Core implementation overview](/implementation/) follows that code, the signal path, and the Nix package functions.
