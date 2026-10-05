/**
 * @file AsyncNats.h
 * @brief NATS client, JetStream key-value store, and object store.
 *
 * The library owns process `main`. The application defines `coMain` and links
 * `AsyncNats::AsyncNats`. This header is the client API. The IO runtime is
 * `<AsyncNats/Core.h>`.
 *
 * Socket operations are not thread-safe. Each connection keeps one outstanding
 * read and one outstanding write on a strand owned by the single IO thread.
 */
#pragma once

#include <boost/cobalt/task.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsyncNats {

/** @brief Return type of `coMain`. */
using Main = boost::cobalt::task<int>;

/**
 * @brief Why a NATS operation failed.
 *
 * `connect`, `closed`, `next`, and `onError` all surface this classification
 * on `Error`.
 */
enum class ErrorKind {
  /** Resolve or the TCP connect failed. */
  unreachable,
  /** The socket died, the peer sent EOF, or the handshake failed. */
  interrupted,
  /** `Client::close()` finished the connection. */
  closed,
  /** The process received `SIGINT` or `SIGTERM`. */
  signal,
  /** A protocol `-ERR`, or a programming mistake such as a missing client. */
  other,
};

/**
 * @brief Failure thrown by the client and passed to `onError`.
 *
 * `what()` is the message. `kind()` says which path produced it.
 */
class Error : public std::runtime_error {
 public:
  /**
   * @param message Text returned by `what()`.
   * @param kind Classification. Defaults to `ErrorKind::other`.
   */
  explicit Error(std::string message, ErrorKind kind = ErrorKind::other)
      : std::runtime_error(std::move(message)), kind_(kind) {}

  /** @brief Which path produced this error. */
  auto kind() const noexcept -> ErrorKind { return kind_; }

 private:
  ErrorKind kind_;
};

/**
 * @brief Extra work run after the library logs a failure or a caught signal.
 *
 * The argument is the library error. On `ErrorKind::signal` the clients are
 * already closed.
 */
using ErrorHandler = std::function<boost::cobalt::task<void>(Error)>;

/**
 * @brief Register the process-wide error handler.
 *
 * Call this before the first `co_await` in `coMain`. Kind `closed` does not
 * run the handler. A handler that is already on the stack is not entered
 * again. An empty handler clears the previous one.
 *
 * On `ErrorKind::signal` the clients are already closed. Waiting on
 * `closed()` again from inside the handler throws into the handler.
 */
void onError(ErrorHandler handler);

class Client;

/** @brief One inbound NATS message. */
struct Message {
  std::string subject;
  std::string payload;
  /** Set when the publisher supplied a reply subject. */
  std::optional<std::string> reply;
};

/**
 * @brief Coroutine started for each message on a route subscription.
 *
 * The read loop starts the handler on the worker pool and does not wait for
 * it. A slow handler does not stall the socket. Client calls from the handler
 * return to the IO thread before they touch the socket. A thrown exception is
 * logged and the connection stays up.
 */
using MessageHandler = std::function<boost::cobalt::task<void>(Message)>;

/** @brief Subject and the handler that receives its messages. */
struct Route {
  std::string subject;
  MessageHandler handler;
};

/**
 * @brief Pull subscription from `Client::subscribe(subject)`.
 *
 * Move-only. Destroying it does not send `UNSUB`. Call `unsubscribe()` to
 * remove it from the server and wake a parked `next()`.
 */
class Subscription {
 public:
  Subscription() = default;
  Subscription(const Subscription&) = delete;
  auto operator=(const Subscription&) -> Subscription& = delete;
  Subscription(Subscription&&) noexcept;
  auto operator=(Subscription&&) noexcept -> Subscription&;
  ~Subscription();

  /**
   * @brief Next message for this subscription.
   *
   * Returns immediately when a message is already queued. Otherwise it
   * suspends until a message arrives, the connection fails, or
   * `unsubscribe()` runs. The per-subscription queue has no limit.
   *
   * @return The next message.
   * @throws Error The connection failed, or this subscription is closed.
   */
  auto next() -> boost::cobalt::task<Message>;

  /**
   * @brief Send `UNSUB` and drop this subscription.
   *
   * A parked `next()` resumes with "subscription closed". A second call does
   * nothing.
   */
  auto unsubscribe() -> boost::cobalt::task<void>;

 private:
  struct State;
  explicit Subscription(std::shared_ptr<State> state);
  std::shared_ptr<State> state_;

  friend class Client;
};

/**
 * @brief One NATS connection.
 *
 * Default-constructed clients are empty and throw `ErrorKind::other` from
 * operations that need a socket. `connect` returns a live client. Copies share
 * the same connection.
 */
class Client {
 public:
  Client() = default;

  /**
   * @brief Publish `payload` to `subject`.
   * @throws Error The client is empty, the subject is invalid, or the write
   * fails.
   */
  auto publish(std::string subject, std::string payload) -> boost::cobalt::task<void>;

  /**
   * @brief Subscribe and return a handle whose `next()` waits for one message.
   * @throws Error The client is empty, the subject is invalid, or the write
   * fails.
   */
  auto subscribe(std::string subject) -> boost::cobalt::task<Subscription>;

  /**
   * @brief Subscribe each route and start its handler for every message.
   *
   * Sends one `SUB` per route. This call does not wait for a message. An empty
   * list does nothing. Duplicate subjects become separate subscriptions. If
   * the write fails, the routes just added are dropped.
   *
   * @throws Error The client is empty, a subject or handler is invalid, or the
   * write fails.
   */
  auto subscribe(std::vector<Route> routes) -> boost::cobalt::task<void>;

  /**
   * @brief Send `UNSUB` for every route on these subjects and drop the handlers.
   *
   * Subjects that were never subscribed are skipped. An empty list does
   * nothing.
   */
  auto unsubscribe(std::vector<std::string> subjects) -> boost::cobalt::task<void>;

  /**
   * @brief Shut the socket down and wake waiters with `ErrorKind::closed`.
   *
   * Does not log and does not run `onError`. An empty client returns
   * immediately.
   */
  auto close() -> boost::cobalt::task<void>;

  /**
   * @brief Suspend until the connection fails or `close()` runs, then throw.
   *
   * The thrown `Error` carries the first reason that ended the connection.
   * A later failure does not replace that reason.
   *
   * @throws Error The stored close reason and kind.
   */
  auto closed() -> boost::cobalt::task<void>;

  /**
   * @brief Publish `payload` and return the body of the reply.
   *
   * Subscribes a one-shot inbox, publishes with that inbox as the reply
   * subject, and waits for one response.
   */
  auto request(std::string subject, std::string payload) -> boost::cobalt::task<std::string>;

  /**
   * @brief `request` with NATS headers.
   *
   * Each pair is a header name and its value.
   */
  auto request(std::string subject, std::string payload,
               std::vector<std::pair<std::string, std::string>> headers)
      -> boost::cobalt::task<std::string>;

 private:
  struct Impl;
  explicit Client(std::shared_ptr<Impl> impl);

  std::shared_ptr<Impl> impl_;

  friend struct Subscription::State;
  friend auto connect(std::string url) -> boost::cobalt::task<Client>;
  friend void onError(ErrorHandler handler);
  friend auto reportError(const Error& failure) -> boost::cobalt::task<void>;
  friend auto closeAllClients(const Error& failure) -> boost::cobalt::task<void>;
  friend void requestStop();
  friend auto stopRequested() -> bool;
};

/**
 * @brief Open a connection to `nats://host:port`.
 *
 * Resolves the host, connects, reads `INFO`, and sends `CONNECT` and `PING`.
 * After `PONG` the read loop owns the socket. A stop request that arrives
 * during connect closes the new socket and throws `ErrorKind::signal`.
 *
 * @param url Server URL. The scheme is `nats://`. The port defaults to 4222.
 * @return A client sharing the new connection.
 * @throws Error Kind `unreachable` when resolve or TCP connect fails, kind
 * `interrupted` when the handshake fails, or kind `signal` when the process
 * is stopping.
 */
auto connect(std::string url) -> boost::cobalt::task<Client>;

/** @brief JetStream key-value and object-store helpers on one connection. */
namespace jetstream {

class Context;

/** @brief Key-value bucket backed by a JetStream stream. */
namespace kv {

/** @brief Settings for a new key-value bucket. */
struct Config {
  std::string bucket;
  /** Revisions kept per key. Clamped to at least 1. Values above 64 throw. */
  std::int64_t history = 1;
};

/** @brief One stored key, its value, and the stream sequence of that value. */
struct Entry {
  std::string key;
  std::string value;
  std::uint64_t revision = 0;
};

/**
 * @brief Handle for one key-value bucket.
 *
 * Operations publish on the bucket stream. A missing or deleted key reads as
 * empty. `remove` publishes a delete marker.
 */
class Store {
 public:
  /**
   * @brief Write `value` at `key`.
   * @return Stream sequence of the new value.
   */
  auto put(std::string key, std::string value) -> boost::cobalt::task<std::uint64_t>;

  /**
   * @brief Write `value` only when `key` is absent or already deleted.
   * @return Stream sequence of the new value.
   * @throws Error The key already holds a value.
   */
  auto create(std::string key, std::string value) -> boost::cobalt::task<std::uint64_t>;

  /**
   * @brief Replace `key` when `revision` is still the latest sequence.
   * @return Stream sequence of the new value.
   * @throws Error The revision does not match.
   */
  auto update(std::string key, std::string value, std::uint64_t revision)
      -> boost::cobalt::task<std::uint64_t>;

  /**
   * @brief Current value of `key`.
   * @return The value, or `std::nullopt` when the key is missing or deleted.
   */
  auto get(std::string key) -> boost::cobalt::task<std::optional<std::string>>;

  /**
   * @brief Current value and revision of `key`.
   * @return The entry, or `std::nullopt` when the key is missing or deleted.
   */
  auto entry(std::string key) -> boost::cobalt::task<std::optional<struct Entry>>;

  /** @brief Publish a delete marker. Older revisions stay in the stream. */
  auto remove(std::string key) -> boost::cobalt::task<void>;

  /** @brief Publish a purge marker and drop the history of `key`. */
  auto purge(std::string key) -> boost::cobalt::task<void>;

 private:
  explicit Store(Client client, std::string bucket);
  Client client_;
  std::string bucket_;

  friend class AsyncNats::jetstream::Context;
};

}  // namespace kv

/** @brief Object store backed by a JetStream stream. */
namespace ObjectStore {

/** @brief Settings for a new object-store bucket. */
struct Config {
  std::string bucket;
};

/** @brief Name and size of a stored object. */
struct ObjectInfo {
  std::string name;
  std::uint64_t size = 0;
  std::uint64_t chunks = 0;
};

/**
 * @brief Handle for one object-store bucket.
 *
 * `put` splits the bytes into chunks. `get` assembles them. `remove` marks
 * the object deleted and purges its chunks.
 */
class Store {
 public:
  /**
   * @brief Store `data` under `name`, replacing a previous object of that name.
   * @return Name, size, and chunk count.
   */
  auto put(std::string name, std::string data) -> boost::cobalt::task<ObjectInfo>;

  /**
   * @brief Bytes stored under `name`.
   * @throws Error The object does not exist.
   */
  auto get(std::string name) -> boost::cobalt::task<std::string>;

  /**
   * @brief Name, size, and chunk count of `name`.
   * @throws Error The object does not exist.
   */
  auto info(std::string name) -> boost::cobalt::task<ObjectInfo>;

  /**
   * @brief Mark `name` deleted and purge its chunks.
   * @throws Error The object does not exist.
   */
  auto remove(std::string name) -> boost::cobalt::task<void>;

 private:
  explicit Store(Client client, std::string bucket);
  Client client_;
  std::string bucket_;

  friend class AsyncNats::jetstream::Context;
};

}  // namespace ObjectStore

/**
 * @brief JetStream entry point for one client.
 *
 * Copies share the underlying connection. Obtain one with `make`.
 */
class Context {
 public:
  /**
   * @brief Create the key-value bucket and return a handle to it.
   * @throws Error The bucket name is invalid, `history` is above 64, or the
   * server rejects the stream.
   */
  auto createKeyValue(kv::Config config) -> boost::cobalt::task<kv::Store>;

  /**
   * @brief Open an existing key-value bucket.
   * @throws Error The bucket name is invalid, or the bucket does not exist.
   */
  auto getKeyValue(std::string bucket) -> boost::cobalt::task<kv::Store>;

  /**
   * @brief Create the object-store bucket and return a handle to it.
   * @throws Error The bucket name is invalid, or the server rejects the stream.
   */
  auto createObjectStore(ObjectStore::Config config) -> boost::cobalt::task<ObjectStore::Store>;

  /**
   * @brief Open an existing object-store bucket.
   * @throws Error The bucket name is invalid, or the bucket does not exist.
   */
  auto getObjectStore(std::string bucket) -> boost::cobalt::task<ObjectStore::Store>;

 private:
  explicit Context(Client client);
  Client client_;

  friend auto make(Client client) -> boost::cobalt::task<Context>;
};

/**
 * @brief JetStream context that shares `client`'s connection.
 *
 * The argument is taken by value. An lvalue client is copied and stays usable.
 */
auto make(Client client) -> boost::cobalt::task<Context>;

}  // namespace jetstream

}  // namespace AsyncNats

/**
 * @brief Application entry the library calls from `main`.
 *
 * Define this in the program that links `AsyncNats::AsyncNats`. The returned
 * integer is logged. The process exits 0 unless an exception escapes.
 * Register `AsyncNats::onError` before the first `co_await`.
 */
auto coMain(int argc, char** argv) -> AsyncNats::Main;
