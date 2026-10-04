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

namespace async_nats {

using main = boost::cobalt::task<int>;

enum class error_kind {
  unreachable,
  interrupted,
  closed,
  signal,
  other,
};

class error : public std::runtime_error {
 public:
  explicit error(std::string message, error_kind kind = error_kind::other)
      : std::runtime_error(std::move(message)), kind_(kind) {}

  auto kind() const noexcept -> error_kind { return kind_; }

 private:
  error_kind kind_;
};

// Runs after the library has logged a connection failure or a SIGINT/SIGTERM.
// On a signal the clients are already closed. The handler can do extra work
// and then return. `error::kind()` says which of those happened.
using error_handler = std::function<boost::cobalt::task<void>(error)>;

void on_error(error_handler handler);

class client;

struct message {
  std::string subject;
  std::string payload;
  std::optional<std::string> reply;
};

using message_handler = std::function<boost::cobalt::task<void>(message)>;

struct route {
  std::string subject;
  message_handler handler;
};

class subscription {
 public:
  subscription() = default;
  subscription(const subscription&) = delete;
  auto operator=(const subscription&) -> subscription& = delete;
  subscription(subscription&&) noexcept;
  auto operator=(subscription&&) noexcept -> subscription&;
  ~subscription();

  auto next() -> boost::cobalt::task<message>;
  auto unsubscribe() -> boost::cobalt::task<void>;

 private:
  struct state;
  explicit subscription(std::shared_ptr<state> state);
  std::shared_ptr<state> state_;

  friend class client;
};

class client {
 public:
  client() = default;

  auto publish(std::string subject, std::string payload) -> boost::cobalt::task<void>;
  auto subscribe(std::string subject) -> boost::cobalt::task<subscription>;
  auto subscribe(std::vector<route> routes) -> boost::cobalt::task<void>;
  auto unsubscribe(std::vector<std::string> subjects) -> boost::cobalt::task<void>;
  auto close() -> boost::cobalt::task<void>;
  auto closed() -> boost::cobalt::task<void>;
  auto request(std::string subject, std::string payload) -> boost::cobalt::task<std::string>;
  auto request(std::string subject, std::string payload,
               std::vector<std::pair<std::string, std::string>> headers) -> boost::cobalt::task<std::string>;

 private:
  struct impl;
  explicit client(std::shared_ptr<impl> impl);

  std::shared_ptr<impl> impl_;

  friend struct subscription::state;
  friend auto connect(std::string url) -> boost::cobalt::task<client>;
  friend void on_error(error_handler handler);
  friend auto report_error(const error& failure) -> boost::cobalt::task<void>;
  friend auto close_all_clients(const error& failure) -> boost::cobalt::task<void>;
  friend void request_stop();
  friend auto stop_requested() -> bool;
};

auto connect(std::string url) -> boost::cobalt::task<client>;

namespace jetstream {

class context;

namespace kv {

struct config {
  std::string bucket;
  std::int64_t history = 1;
};

struct entry {
  std::string key;
  std::string value;
  std::uint64_t revision = 0;
};

class store {
 public:
  auto put(std::string key, std::string value) -> boost::cobalt::task<std::uint64_t>;
  auto create(std::string key, std::string value) -> boost::cobalt::task<std::uint64_t>;
  auto update(std::string key, std::string value, std::uint64_t revision) -> boost::cobalt::task<std::uint64_t>;
  auto get(std::string key) -> boost::cobalt::task<std::optional<std::string>>;
  auto entry(std::string key) -> boost::cobalt::task<std::optional<struct entry>>;
  auto delete_(std::string key) -> boost::cobalt::task<void>;
  auto purge(std::string key) -> boost::cobalt::task<void>;

 private:
  explicit store(client client, std::string bucket);
  client client_;
  std::string bucket_;

  friend class async_nats::jetstream::context;
};

}  // namespace kv

namespace object_store {

struct config {
  std::string bucket;
};

struct object_info {
  std::string name;
  std::uint64_t size = 0;
  std::uint64_t chunks = 0;
};

class store {
 public:
  auto put(std::string name, std::string data) -> boost::cobalt::task<object_info>;
  auto get(std::string name) -> boost::cobalt::task<std::string>;
  auto info(std::string name) -> boost::cobalt::task<object_info>;
  auto delete_(std::string name) -> boost::cobalt::task<void>;

 private:
  explicit store(client client, std::string bucket);
  client client_;
  std::string bucket_;

  friend class async_nats::jetstream::context;
};

}  // namespace object_store

class context {
 public:
  auto create_key_value(kv::config config) -> boost::cobalt::task<kv::store>;
  auto get_key_value(std::string bucket) -> boost::cobalt::task<kv::store>;
  auto create_object_store(object_store::config config) -> boost::cobalt::task<object_store::store>;
  auto get_object_store(std::string bucket) -> boost::cobalt::task<object_store::store>;

 private:
  explicit context(client client);
  client client_;

  friend auto make(client client) -> boost::cobalt::task<context>;
};

auto make(client client) -> boost::cobalt::task<context>;

}  // namespace jetstream

}  // namespace async_nats

auto co_main(int argc, char** argv) -> async_nats::main;
