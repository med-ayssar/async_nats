#include <AsyncNats/client/AsyncNats.h>
#include <AsyncNats/client/Session.h>
#include <AsyncNats/core/Core.h>

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
#include <boost/system/system_error.hpp>
#include <spdlog/spdlog.h>

#include <coroutine>
#include <cstdint>
#include <deque>
#include <exception>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AsyncNats {
namespace {

struct Endpoint {
  std::string host;
  std::string port;
};

auto parseUrl(std::string url) -> Endpoint {
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

void requireSubject(std::string_view subject) {
  if (subject.empty() || subject.find_first_of(" \r\n") != std::string_view::npos) {
    throw Error("invalid subject");
  }
}

auto interruptedError(const std::string& target, std::string_view reason) -> Error {
  return Error("connection to NATS server " + target + " was interrupted: " + std::string(reason),
               ErrorKind::interrupted);
}

auto unreachableError(const std::string& target, const boost::system::system_error& failure) -> Error {
  return Error("NATS server at " + target + " is not reachable: " + failure.code().message(),
               ErrorKind::unreachable);
}

}  // namespace

struct Client::Impl {
  struct Inbound {
    std::uint64_t sid = 0;
    Message message;
  };

  struct Pending {
    std::coroutine_handle<> handle;
    Message* result = nullptr;
    bool* failed = nullptr;
    std::string* reason = nullptr;
  };

  boost::asio::ip::tcp::socket socket;
  std::string target;
  std::string buffer;
  bool write_busy = false;
  std::deque<std::coroutine_handle<>> write_waiters;
  std::uint64_t next_sid = 1;
  std::unordered_map<std::uint64_t, std::deque<Message>> queues;
  std::unordered_map<std::uint64_t, Pending> waiters;
  std::unordered_map<std::uint64_t, MessageHandler> handlers;
  std::unordered_map<std::string, std::vector<std::uint64_t>> routes;
  std::vector<std::coroutine_handle<>> close_waiters;
  bool failed = false;
  std::string fail_reason;
  ErrorKind fail_kind = ErrorKind::closed;

  explicit Impl(boost::asio::ip::tcp::socket socket, std::string target)
      : socket(std::move(socket)), target(std::move(target)) {}

  auto enter() -> boost::cobalt::task<void> {
    co_await boost::asio::dispatch(socket.get_executor(), boost::cobalt::use_op);
  }

  auto acquireWrite() -> boost::cobalt::task<void> {
    if (!write_busy) {
      write_busy = true;
      co_return;
    }
    struct Wait {
      Impl* self = nullptr;
      bool await_ready() const noexcept { return false; }
      void await_suspend(std::coroutine_handle<> handle) { self->write_waiters.push_back(handle); }
      void await_resume() const noexcept {}
    };
    co_await Wait{this};
  }

  void releaseWrite() {
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
    co_await acquireWrite();
    std::optional<Error> failure;
    try {
      co_await boost::asio::async_write(socket, boost::asio::buffer(bytes), boost::cobalt::use_op);
    } catch (const boost::system::system_error& ex) {
      releaseWrite();
      if (!failed && ex.code() != boost::asio::error::operation_aborted) {
        failure = interruptedError(target, ex.code().message());
      } else {
        throw;
      }
    } catch (...) {
      releaseWrite();
      throw;
    }
    if (failure) {
      failWaiters(failure->what(), failure->kind());
      co_await reportError(*failure);
      throw *failure;
    }
    releaseWrite();
  }

  auto readLine() -> boost::cobalt::task<std::string> {
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

  auto readExact(std::size_t size) -> boost::cobalt::task<void> {
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

  auto readIncoming() -> boost::cobalt::task<Inbound> {
    for (;;) {
      auto line = co_await readLine();
      if (line == "PING") {
        co_await write("PONG\r\n");
        continue;
      }
      if (line.starts_with("-ERR")) {
        throw Error(line);
      }
      if (!line.starts_with("MSG ") && !line.starts_with("HMSG ")) {
        continue;
      }

      const bool headers = line.starts_with("HMSG ");
      const auto parts = split(line);
      if (parts.size() < 4) {
        throw Error("malformed NATS message");
      }

      Inbound incoming;
      incoming.message.subject = std::string(parts[1]);
      incoming.sid = std::stoull(std::string(parts[2]));
      const auto size = static_cast<std::size_t>(std::stoul(std::string(parts.back())));
      if ((!headers && parts.size() == 5) || (headers && parts.size() == 6)) {
        incoming.message.reply = std::string(parts[3]);
      }

      co_await readExact(size + 2);
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

  void forgetRoute(std::uint64_t sid, const std::string& subject) {
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

  void deliver(Inbound incoming) {
    if (auto route = handlers.find(incoming.sid); route != handlers.end()) {
      try {
        auto handled = route->second(std::move(incoming.message));
        boost::cobalt::spawn(
            Core::instance().threadPool().get_executor(), std::move(handled), [](std::exception_ptr exception) {
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

  void failWaiters(std::string reason, ErrorKind kind) {
    if (failed) {
      return;
    }
    failed = true;
    fail_reason = std::move(reason);
    fail_kind = kind;
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

  auto receive(std::uint64_t sid) -> boost::cobalt::task<Message> {
    co_await enter();
    if (auto queued = queues.find(sid); queued != queues.end() && !queued->second.empty()) {
      auto message = std::move(queued->second.front());
      queued->second.pop_front();
      co_return message;
    }
    if (failed) {
      throw Error(fail_reason.empty() ? "connection closed" : fail_reason, fail_kind);
    }

    struct Wait {
      Impl* self = nullptr;
      std::uint64_t sid = 0;
      Message result;
      bool failed = false;
      std::string reason;
      bool await_ready() const noexcept { return false; }
      void await_suspend(std::coroutine_handle<> handle) {
        self->waiters[sid] = Pending{handle, &result, &failed, &reason};
      }
      void await_resume() const {
        if (failed) {
          throw Error(reason.empty() ? "subscription closed" : reason);
        }
      }
    };

    Wait operation{this, sid};
    co_await operation;
    co_return std::move(operation.result);
  }

  auto shutdown(std::string reason, ErrorKind kind) -> boost::cobalt::task<void> {
    co_await enter();
    if (failed && !socket.is_open()) {
      co_return;
    }
    handlers.clear();
    routes.clear();
    boost::system::error_code ignored;
    socket.cancel(ignored);
    socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
    failWaiters(std::move(reason), kind);
  }

  struct Registry {
    ErrorHandler handler;
    std::vector<std::weak_ptr<Impl>> clients;
    bool stopping = false;
    bool in_handler = false;
  };

  static auto sessions() -> Registry& {
    static Registry state;
    return state;
  }

  static void track(const std::shared_ptr<Impl>& connection) {
    std::erase_if(sessions().clients, [](const std::weak_ptr<Impl>& client) { return client.expired(); });
    sessions().clients.push_back(connection);
  }

  static auto closeAll(const Error& failure) -> boost::cobalt::task<void> {
    std::vector<std::shared_ptr<Impl>> open;
    for (auto& client : sessions().clients) {
      if (auto connection = client.lock()) {
        open.push_back(std::move(connection));
      }
    }
    sessions().clients.clear();
    for (auto& connection : open) {
      co_await connection->shutdown(failure.what(), failure.kind());
    }
  }

  auto readLoop(std::shared_ptr<Impl> self) -> boost::cobalt::task<void> {
    std::optional<Error> failure;
    try {
      for (;;) {
        auto incoming = co_await self->readIncoming();
        self->deliver(std::move(incoming));
      }
    } catch (const Error& ex) {
      if (!self->failed) {
        failure = ex;
      }
    } catch (const boost::system::system_error& ex) {
      if (!self->failed && ex.code() != boost::asio::error::operation_aborted) {
        failure = interruptedError(self->target, ex.code().message());
      }
    } catch (const std::exception& ex) {
      if (!self->failed) {
        failure = interruptedError(self->target, ex.what());
      }
    }
    if (!failure) {
      co_return;
    }
    self->failWaiters(failure->what(), failure->kind());
    co_await reportError(*failure);
  }
};

auto reportError(const Error& failure) -> boost::cobalt::task<void> {
  if (failure.kind() == ErrorKind::signal) {
    spdlog::info("{}", failure.what());
  } else if (failure.kind() != ErrorKind::closed) {
    spdlog::error("{}", failure.what());
  }
  auto handler = Client::Impl::sessions().handler;
  if (!handler || Client::Impl::sessions().in_handler) {
    co_return;
  }
  Client::Impl::sessions().in_handler = true;
  struct Clear {
    bool& flag;
    ~Clear() { flag = false; }
  } guard{Client::Impl::sessions().in_handler};
  try {
    co_await handler(failure);
  } catch (const std::exception& ex) {
    spdlog::error("nats error handler failed: {}", ex.what());
  }
}

void onError(ErrorHandler handler) { Client::Impl::sessions().handler = std::move(handler); }

void requestStop() { Client::Impl::sessions().stopping = true; }

auto stopRequested() -> bool { return Client::Impl::sessions().stopping; }

auto closeAllClients(const Error& failure) -> boost::cobalt::task<void> {
  co_await Client::Impl::closeAll(failure);
}

struct Subscription::State {
  std::shared_ptr<Client::Impl> connection;
  std::uint64_t sid = 0;
  bool open = false;
};

Subscription::Subscription(std::shared_ptr<State> state) : state_(std::move(state)) {}

Subscription::Subscription(Subscription&&) noexcept = default;

auto Subscription::operator=(Subscription&&) noexcept -> Subscription& = default;

Subscription::~Subscription() = default;

Client::Client(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

auto connect(std::string url) -> boost::cobalt::task<Client> {
  if (stopRequested()) {
    throw Error("received signal, closing NATS client", ErrorKind::signal);
  }
  const auto where = parseUrl(std::move(url));
  const auto target = where.host + ":" + where.port;
  auto executor = co_await boost::cobalt::this_coro::executor;
  auto resolver = boost::asio::ip::tcp::resolver{executor};

  std::optional<Error> failure;
  boost::asio::ip::tcp::resolver::results_type results;
  try {
    results = co_await resolver.async_resolve(where.host, where.port, boost::cobalt::use_op);
  } catch (const boost::system::system_error& ex) {
    failure = unreachableError(target, ex);
  } catch (const std::exception& ex) {
    failure = Error("NATS server at " + target + " is not reachable: " + ex.what(), ErrorKind::unreachable);
  }
  if (failure) {
    co_await reportError(*failure);
    throw *failure;
  }
  if (stopRequested()) {
    throw Error("received signal, closing NATS client", ErrorKind::signal);
  }

  auto strand = boost::asio::make_strand(executor);
  boost::asio::ip::tcp::socket socket{strand};
  co_await boost::asio::dispatch(socket.get_executor(), boost::cobalt::use_op);
  try {
    co_await boost::asio::async_connect(socket, results, boost::cobalt::use_op);
  } catch (const boost::system::system_error& ex) {
    failure = unreachableError(target, ex);
  } catch (const std::exception& ex) {
    failure = Error("NATS server at " + target + " is not reachable: " + ex.what(), ErrorKind::unreachable);
  }
  if (failure) {
    co_await reportError(*failure);
    throw *failure;
  }

  auto state = std::make_shared<Client::Impl>(std::move(socket), target);
  if (stopRequested()) {
    co_await state->shutdown("received signal, closing NATS client", ErrorKind::signal);
    throw Error("received signal, closing NATS client", ErrorKind::signal);
  }

  std::optional<Error> reported;
  try {
    const auto info = co_await state->readLine();
    if (!info.starts_with("INFO ")) {
      failure = Error("NATS server at " + target + " did not send INFO", ErrorKind::interrupted);
    } else {
      co_await state->write(
          "CONNECT {\"verbose\":false,\"pedantic\":false,\"tls_required\":false,"
          "\"lang\":\"cpp\",\"version\":\"0.1.0\",\"protocol\":1}\r\nPING\r\n");
      for (;;) {
        auto line = co_await state->readLine();
        if (line == "PONG") {
          break;
        }
        if (line == "PING") {
          co_await state->write("PONG\r\n");
          continue;
        }
        if (line.starts_with("-ERR")) {
          failure = Error(std::move(line), ErrorKind::interrupted);
          break;
        }
      }
    }
  } catch (const Error& ex) {
    reported = ex;
  } catch (const boost::system::system_error& ex) {
    failure = interruptedError(target, ex.code().message());
  } catch (const std::exception& ex) {
    failure = interruptedError(target, ex.what());
  }
  if (reported) {
    if (!state->failed) {
      co_await state->shutdown(reported->what(), reported->kind());
      co_await reportError(*reported);
    }
    throw *reported;
  }
  if (failure) {
    co_await state->shutdown(failure->what(), failure->kind());
    co_await reportError(*failure);
    throw *failure;
  }
  if (stopRequested()) {
    co_await state->shutdown("received signal, closing NATS client", ErrorKind::signal);
    throw Error("received signal, closing NATS client", ErrorKind::signal);
  }

  Client::Impl::track(state);
  boost::cobalt::spawn(state->socket.get_executor(), state->readLoop(state), [](std::exception_ptr exception) {
    if (!exception) {
      return;
    }
    try {
      std::rethrow_exception(exception);
    } catch (const std::exception& ex) {
      spdlog::debug("nats read loop ended: {}", ex.what());
    }
  });

  co_return Client{std::move(state)};
}

auto Client::publish(std::string subject, std::string payload) -> boost::cobalt::task<void> {
  if (!impl_) {
    throw Error("client is not connected");
  }
  requireSubject(subject);
  co_await impl_->write("PUB " + subject + " " + std::to_string(payload.size()) + "\r\n" + payload + "\r\n");
}

auto Client::subscribe(std::string subject) -> boost::cobalt::task<Subscription> {
  if (!impl_) {
    throw Error("client is not connected");
  }
  requireSubject(subject);
  co_await impl_->enter();
  const auto sid = impl_->next_sid++;
  co_await impl_->write("SUB " + subject + " " + std::to_string(sid) + "\r\n");
  auto state = std::make_shared<Subscription::State>();
  state->connection = impl_;
  state->sid = sid;
  state->open = true;
  co_return Subscription{std::move(state)};
}

auto Client::subscribe(std::vector<Route> routes) -> boost::cobalt::task<void> {
  if (!impl_) {
    throw Error("client is not connected");
  }
  for (const auto& route : routes) {
    requireSubject(route.subject);
    if (!route.handler) {
      throw Error("invalid handler");
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
      impl_->forgetRoute(sid, subject);
    }
    throw;
  }
}

auto Client::unsubscribe(std::vector<std::string> subjects) -> boost::cobalt::task<void> {
  if (!impl_) {
    throw Error("client is not connected");
  }
  for (const auto& subject : subjects) {
    requireSubject(subject);
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

auto Client::close() -> boost::cobalt::task<void> {
  if (!impl_) {
    co_return;
  }
  co_await impl_->shutdown("connection closed", ErrorKind::closed);
}

auto Client::closed() -> boost::cobalt::task<void> {
  if (!impl_) {
    throw Error("client is not connected");
  }
  co_await impl_->enter();
  if (impl_->failed) {
    throw Error(impl_->fail_reason.empty() ? "connection closed" : impl_->fail_reason, impl_->fail_kind);
  }

  struct Wait {
    Impl* self = nullptr;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> handle) { self->close_waiters.push_back(handle); }
    void await_resume() const noexcept {}
  };

  co_await Wait{impl_.get()};
  throw Error(impl_->fail_reason.empty() ? "connection closed" : impl_->fail_reason, impl_->fail_kind);
}

auto Subscription::next() -> boost::cobalt::task<Message> {
  if (!state_ || !state_->open || !state_->connection) {
    throw Error("subscription is closed");
  }
  co_return co_await state_->connection->receive(state_->sid);
}

auto Subscription::unsubscribe() -> boost::cobalt::task<void> {
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

auto Client::request(std::string subject, std::string payload) -> boost::cobalt::task<std::string> {
  return request(std::move(subject), std::move(payload), {});
}

auto Client::request(std::string subject, std::string payload,
                     std::vector<std::pair<std::string, std::string>> headers) -> boost::cobalt::task<std::string> {
  if (!impl_) {
    throw Error("client is not connected");
  }
  requireSubject(subject);
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

}  // namespace AsyncNats
