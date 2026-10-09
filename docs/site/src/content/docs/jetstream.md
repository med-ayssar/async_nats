---
title: JetStream
description: Key-value buckets and the object store on one client connection.
---

The generated pages start at [`jetstream`](/doxygen/namespace_async_nats_1_1jetstream.html). `jetstream::make` returns a context that shares the client's connection. An lvalue client is copied and stays usable.

```cpp
auto js = co_await AsyncNats::jetstream::make(client);

auto kv = co_await js.createKeyValue({.bucket = "store", .history = 10});
co_await kv.put("key", "value");
auto value = co_await kv.get("key");

auto objects = co_await js.createObjectStore({.bucket = "files"});
co_await objects.put("file", "hello");
auto data = co_await objects.get("file");
```

`getKeyValue` and `getObjectStore` open a bucket that already exists. A missing bucket throws `bucket not found`.

## Key-value

`kv::Config` is a bucket name and `history`. History defaults to 1, is clamped to at least 1, and throws above 64.

| Method | Behavior |
| --- | --- |
| `put` | Write the value. Returns the stream sequence. |
| `create` | Write only when the key is absent or already deleted. Throws `key already exists` otherwise. |
| `update` | Replace the key when `revision` is still the latest sequence. |
| `get` | The current value, or `nullopt` when the key is missing or deleted. |
| `entry` | Key, value, and revision, or `nullopt` when the key is missing or deleted. |
| `remove` | Publish a delete marker. Older revisions stay. |
| `purge` | Publish a purge marker and drop the history of that key. |

## Object store

`put` splits the bytes into chunks and replaces a previous object of the same name. It returns `ObjectInfo`: name, size, and chunk count.

`get` assembles the bytes. `info` returns the same metadata. Both throw `object not found` when the name is missing. `remove` marks the object deleted and purges its chunks.
