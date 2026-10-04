#include <async_nats.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>
#include <boost/cobalt/op.hpp>
#include <boost/cobalt/spawn.hpp>
#include <boost/cobalt/this_coro.hpp>
#include <spdlog/spdlog.h>

#include <coroutine>
#include <cstdint>
#include <deque>
#include <exception>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace async_nats {
namespace {

struct endpoint {
  std::string host;
  std::string port;
};

auto parse_url(std::string url) -> endpoint {
  constexpr std::string_view scheme = "nats://";
  if (url.starts_with(scheme)) {
    url.erase(0, scheme.size());
  }
  const auto slash = url.find('/');
  if (slash != std::string::npos) {
    url.resize(slash);
  }
  const auto colon = url.rfind(':');
  if (colon == std::string::npos) {
    return {std::move(url), "4222"};
  }
  return {url.substr(0, colon), url.substr(colon + 1)};
}

auto split(std::string_view line) -> std::vector<std::string_view> {
  std::vector<std::string_view> parts;
  std::size_t index = 0;
  while (index < line.size()) {
    while (index < line.size() && line[index] == ' ') {
      ++index;
    }
    if (index >= line.size()) {
      break;
    }
    const auto start = index;
    while (index < line.size() && line[index] != ' ') {
      ++index;
    }
    parts.emplace_back(line.substr(start, index - start));
  }
  return parts;
}

void require_subject(std::string_view subject) {
  if (subject.empty() || subject.find_first_of(" \r\n") != std::string_view::npos) {
    throw error("invalid subject");
  }
}

}  // namespace

struct client::impl {
  struct inbound {
    std::uint64_t sid = 0;
    message message;
  };

  struct pending {
    std::coroutine_handle<> handle;
    message* result = nullptr;
    bool* failed = nullptr;
    std::string* reason = nullptr;
  };

  boost::asio::ip::tcp::socket socket;
  std::string buffer;
  bool write_busy = false;
  std::deque<std::coroutine_handle<>> write_waiters;
  std::uint64_t next_sid = 1;
  std::unordered_map<std::uint64_t, std::deque<message>> queues;
  std::unordered_map<std::uint64_t, pending> waiters;
  std::unordered_map<std::uint64_t, message_handler> handlers;
  std::unordered_map<std::string, std::vector<std::uint64_t>> routes;
  std::vector<std::coroutine_handle<>> close_waiters;
  bool failed = false;
  std::string fail_reason;

  explicit impl(boost::asio::ip::tcp::socket socket) : socket(std::move(socket)) {}

  auto enter() -> boost::cobalt::task<void> {
    co_await boost::asio::dispatch(socket.get_executor(), boost::cobalt::use_op);
  }

  auto acquire_write() -> boost::cobalt::task<void> {
    if (!write_busy) {
      write_busy = true;
      co_return;
    }
    struct wait {
      impl* self = nullptr;
      bool await_ready() const noexcept { return false; }
      void await_suspend(std::coroutine_handle<> handle) { self->write_waiters.push_back(handle); }
      void await_resume() const noexcept {}
    };
    co_await wait{this};
  }

  void release_write() {
    if (write_waiters.empty()) {
      write_busy = false;
      return;
    }
    auto handle = write_waiters.front();
    write_waiters.pop_front();
    boost::asio::post(socket.get_executor(), [handle] { handle.resume(); });
  }

  auto write(std::string bytes) -> boost::cobalt::task<void> {
    co_await enter();
    co_await acquire_write();
    try {
      co_await boost::asio::async_write(socket, boost::asio::buffer(bytes), boost::cobalt::use_op);
    } catch (...) {
      release_write();
      throw;
    }
    release_write();
  }

  auto read_line() -> boost::cobalt::task<std::string> {
    for (;;) {
      const auto mark = buffer.find("\r\n");
      if (mark != std::string::npos) {
        auto line = buffer.substr(0, mark);
        buffer.erase(0, mark + 2);
        co_return line;
      }
      co_await boost::asio::async_read_until(socket, boost::asio::dynamic_buffer(buffer), "\r\n",
                                              boost::cobalt::use_op);
    }
  }

  auto read_exact(std::size_t size) -> boost::cobalt::task<void> {
    while (buffer.size() < size) {
      co_await boost::asio::async_read_until(socket, boost::asio::dynamic_buffer(buffer), "\r\n",
                                              boost::cobalt::use_op);
      if (buffer.size() < size) {
        co_await boost::asio::async_read(socket, boost::asio::dynamic_buffer(buffer),
                                          boost::asio::transfer_at_least(size - buffer.size()),
                                          boost::cobalt::use_op);
      }
    }
  }

  auto read_incoming() -> boost::cobalt::task<inbound> {
    for (;;) {
      auto line = co_await read_line();
      if (line == "PING") {
        co_await write("PONG\r\n");
        continue;
      }
      if (line.starts_with("-ERR")) {
        throw error(line);
      }
      if (!line.starts_with("MSG ") && !line.starts_with("HMSG ")) {
        continue;
      }

      const bool headers = line.starts_with("HMSG ");
      const auto parts = split(line);
      if (parts.size() < 4) {
        throw error("malformed NATS message");
      }

      inbound incoming;
      incoming.message.subject = std::string(parts[1]);
      incoming.sid = std::stoull(std::string(parts[2]));
      const auto size = static_cast<std::size_t>(std::stoul(std::string(parts.back())));
      if ((!headers && parts.size() == 5) || (headers && parts.size() == 6)) {
        incoming.message.reply = std::string(parts[3]);
      }

      co_await read_exact(size + 2);
      incoming.message.payload = buffer.substr(0, size);
      buffer.erase(0, size + 2);
      if (headers) {
        const auto header_end = incoming.message.payload.find("\r\n\r\n");
        if (header_end != std::string::npos) {
          incoming.message.payload.erase(0, header_end + 4);
        }
      }
      co_return incoming;
    }
  }

  void forget_route(std::uint64_t sid, const std::string& subject) {
    handlers.erase(sid);
    auto found = routes.find(subject);
    if (found == routes.end()) {
      return;
    }
    std::erase(found->second, sid);
    if (found->second.empty()) {
      routes.erase(found);
    }
  }

  void deliver(inbound incoming) {
    if (auto route = handlers.find(incoming.sid); route != handlers.end()) {
      try {
        auto handled = route->second(std::move(incoming.message));
        boost::cobalt::spawn(socket.get_executor(), std::move(handled), [](std::exception_ptr exception) {
          if (!exception) {
            return;
          }
          try {
            std::rethrow_exception(exception);
          } catch (const std::exception& ex) {
            spdlog::error("nats handler failed: {}", ex.what());
          }
        });
      } catch (const std::exception& ex) {
        spdlog::error("nats handler failed: {}", ex.what());
      }
      return;
    }
    auto found = waiters.find(incoming.sid);
    if (found != waiters.end()) {
      *found->second.result = std::move(incoming.message);
      auto handle = found->second.handle;
      waiters.erase(found);
      boost::asio::post(socket.get_executor(), [handle] { handle.resume(); });
      return;
    }
    queues[incoming.sid].push_back(std::move(incoming.message));
  }

  void fail_waiters(std::string reason) {
    failed = true;
    fail_reason = std::move(reason);
    std::vector<std::coroutine_handle<>> handles;
    handles.reserve(waiters.size());
    for (auto& [sid, waiter] : waiters) {
      (void)sid;
      *waiter.failed = true;
      *waiter.reason = fail_reason;
      handles.push_back(waiter.handle);
    }
    waiters.clear();
    auto closing = std::move(close_waiters);
    close_waiters.clear();
    handles.insert(handles.end(), closing.begin(), closing.end());
    for (auto handle : handles) {
      boost::asio::post(socket.get_executor(), [handle] { handle.resume(); });
    }
  }

  auto receive(std::uint64_t sid) -> boost::cobalt::task<message> {
    co_await enter();
    if (auto queued = queues.find(sid); queued != queues.end() && !queued->second.empty()) {
      auto message = std::move(queued->second.front());
      queued->second.pop_front();
      co_return message;
    }
    if (failed) {
      throw error(fail_reason.empty() ? "connection closed" : fail_reason);
    }

    struct wait {
      impl* self = nullptr;
      std::uint64_t sid = 0;
      message result;
      bool failed = false;
      std::string reason;
      bool await_ready() const noexcept { return false; }
      void await_suspend(std::coroutine_handle<> handle) {
        self->waiters[sid] = pending{handle, &result, &failed, &reason};
      }
      void await_resume() const {
        if (failed) {
          throw error(reason.empty() ? "subscription closed" : reason);
        }
      }
    };

    wait operation{this, sid};
    co_await operation;
    co_return std::move(operation.result);
  }

  auto read_loop(std::shared_ptr<impl> self) -> boost::cobalt::task<void> {
    try {
      for (;;) {
        auto incoming = co_await self->read_incoming();
        self->deliver(std::move(incoming));
      }
    } catch (const std::exception& ex) {
      self->fail_waiters(ex.what());
    }
  }
};

struct subscription::state {
  std::shared_ptr<client::impl> connection;
  std::uint64_t sid = 0;
  bool open = false;
};

subscription::subscription(std::shared_ptr<state> state) : state_(std::move(state)) {}

subscription::subscription(subscription&&) noexcept = default;

auto subscription::operator=(subscription&&) noexcept -> subscription& = default;

subscription::~subscription() = default;

client::client(std::shared_ptr<impl> impl) : impl_(std::move(impl)) {}

auto connect(std::string url) -> boost::cobalt::task<client> {
  const auto where = parse_url(std::move(url));
  auto executor = co_await boost::cobalt::this_coro::executor;
  auto resolver = boost::asio::ip::tcp::resolver{executor};
  auto results = co_await resolver.async_resolve(where.host, where.port, boost::cobalt::use_op);

  auto strand = boost::asio::make_strand(executor);
  boost::asio::ip::tcp::socket socket{strand};
  co_await boost::asio::dispatch(socket.get_executor(), boost::cobalt::use_op);
  co_await boost::asio::async_connect(socket, results, boost::cobalt::use_op);

  auto state = std::make_shared<client::impl>(std::move(socket));
  const auto info = co_await state->read_line();
  if (!info.starts_with("INFO ")) {
    throw error("NATS server did not send INFO");
  }
  co_await state->write(
      "CONNECT {\"verbose\":false,\"pedantic\":false,\"tls_required\":false,"
      "\"lang\":\"cpp\",\"version\":\"0.1.0\",\"protocol\":1}\r\nPING\r\n");

  for (;;) {
    auto line = co_await state->read_line();
    if (line == "PONG") {
      break;
    }
    if (line == "PING") {
      co_await state->write("PONG\r\n");
      continue;
    }
    if (line.starts_with("-ERR")) {
      throw error(line);
    }
  }

  boost::cobalt::spawn(state->socket.get_executor(), state->read_loop(state), [](std::exception_ptr exception) {
    if (!exception) {
      return;
    }
    try {
      std::rethrow_exception(exception);
    } catch (const std::exception& ex) {
      spdlog::debug("nats read loop ended: {}", ex.what());
    }
  });

  co_return client{std::move(state)};
}

auto client::publish(std::string subject, std::string payload) -> boost::cobalt::task<void> {
  if (!impl_) {
    throw error("client is not connected");
  }
  require_subject(subject);
  co_await impl_->write("PUB " + subject + " " + std::to_string(payload.size()) + "\r\n" + payload + "\r\n");
}

auto client::subscribe(std::string subject) -> boost::cobalt::task<subscription> {
  if (!impl_) {
    throw error("client is not connected");
  }
  require_subject(subject);
  co_await impl_->enter();
  const auto sid = impl_->next_sid++;
  co_await impl_->write("SUB " + subject + " " + std::to_string(sid) + "\r\n");
  auto state = std::make_shared<subscription::state>();
  state->connection = impl_;
  state->sid = sid;
  state->open = true;
  co_return subscription{std::move(state)};
}

auto client::subscribe(std::vector<route> routes) -> boost::cobalt::task<void> {
  if (!impl_) {
    throw error("client is not connected");
  }
  for (const auto& route : routes) {
    require_subject(route.subject);
    if (!route.handler) {
      throw error("invalid handler");
    }
  }
  if (routes.empty()) {
    co_return;
  }

  co_await impl_->enter();
  std::string frame;
  std::vector<std::pair<std::uint64_t, std::string>> added;
  added.reserve(routes.size());
  for (auto& route : routes) {
    const auto sid = impl_->next_sid++;
    frame += "SUB " + route.subject + " " + std::to_string(sid) + "\r\n";
    impl_->routes[route.subject].push_back(sid);
    impl_->handlers.emplace(sid, std::move(route.handler));
    added.emplace_back(sid, route.subject);
  }
  try {
    co_await impl_->write(std::move(frame));
  } catch (...) {
    for (const auto& [sid, subject] : added) {
      impl_->forget_route(sid, subject);
    }
    throw;
  }
}

auto client::unsubscribe(std::vector<std::string> subjects) -> boost::cobalt::task<void> {
  if (!impl_) {
    throw error("client is not connected");
  }
  for (const auto& subject : subjects) {
    require_subject(subject);
  }
  if (subjects.empty()) {
    co_return;
  }

  co_await impl_->enter();
  std::string frame;
  std::vector<std::pair<std::string, std::vector<std::uint64_t>>> closing;
  for (const auto& subject : subjects) {
    auto found = impl_->routes.find(subject);
    if (found == impl_->routes.end()) {
      continue;
    }
    for (const auto sid : found->second) {
      frame += "UNSUB " + std::to_string(sid) + "\r\n";
    }
    closing.emplace_back(subject, found->second);
  }
  if (!frame.empty()) {
    co_await impl_->write(std::move(frame));
  }
  for (const auto& [subject, sids] : closing) {
    for (const auto sid : sids) {
      impl_->handlers.erase(sid);
      impl_->queues.erase(sid);
    }
    impl_->routes.erase(subject);
  }
}

auto client::closed() -> boost::cobalt::task<void> {
  if (!impl_) {
    throw error("client is not connected");
  }
  co_await impl_->enter();
  if (impl_->failed) {
    throw error(impl_->fail_reason.empty() ? "connection closed" : impl_->fail_reason);
  }

  struct wait {
    impl* self = nullptr;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> handle) { self->close_waiters.push_back(handle); }
    void await_resume() const noexcept {}
  };

  co_await wait{impl_.get()};
  throw error(impl_->fail_reason.empty() ? "connection closed" : impl_->fail_reason);
}

auto subscription::next() -> boost::cobalt::task<message> {
  if (!state_ || !state_->open || !state_->connection) {
    throw error("subscription is closed");
  }
  co_return co_await state_->connection->receive(state_->sid);
}

auto subscription::unsubscribe() -> boost::cobalt::task<void> {
  if (!state_ || !state_->open || !state_->connection) {
    co_return;
  }
  state_->open = false;
  auto connection = state_->connection;
  const auto sid = state_->sid;
  co_await connection->write("UNSUB " + std::to_string(sid) + "\r\n");
  connection->queues.erase(sid);
  if (auto waiter = connection->waiters.find(sid); waiter != connection->waiters.end()) {
    *waiter->second.failed = true;
    *waiter->second.reason = "subscription closed";
    auto handle = waiter->second.handle;
    connection->waiters.erase(waiter);
    boost::asio::post(connection->socket.get_executor(), [handle] { handle.resume(); });
  }
  state_->connection.reset();
}

auto client::request(std::string subject, std::string payload) -> boost::cobalt::task<std::string> {
  return request(std::move(subject), std::move(payload), {});
}

auto client::request(std::string subject, std::string payload,
                     std::vector<std::pair<std::string, std::string>> headers) -> boost::cobalt::task<std::string> {
  if (!impl_) {
    throw error("client is not connected");
  }
  require_subject(subject);
  co_await impl_->enter();
  const auto sid = impl_->next_sid++;
  const auto inbox = "_INBOX." + std::to_string(sid);
  const auto id = std::to_string(sid);
  std::string frame = "SUB " + inbox + " " + id + "\r\n";
  if (headers.empty()) {
    frame += "PUB " + subject + " " + inbox + " " + std::to_string(payload.size()) + "\r\n" + payload + "\r\n";
  } else {
    std::string block = "NATS/1.0\r\n";
    for (const auto& [name, value] : headers) {
      block += name;
      block += ": ";
      block += value;
      block += "\r\n";
    }
    block += "\r\n";
    const auto total = block.size() + payload.size();
    frame += "HPUB " + subject + " " + inbox + " " + std::to_string(block.size()) + " " + std::to_string(total) +
             "\r\n" + block + payload + "\r\n";
  }
  frame += "UNSUB " + id + " 1\r\n";
  co_await impl_->write(std::move(frame));
  auto response = co_await impl_->receive(sid);
  co_return std::move(response.payload);
}

}  // namespace async_nats
