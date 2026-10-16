#include <AsyncNats/client.h>
#include <AsyncNats/client/Session.h>
#include <AsyncNats/core/Core.h>
#include <AsyncNats/eventloop/NatsEventLoop.h>
#include <nats/nats.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/cobalt/op.hpp>
#include <boost/cobalt/spawn.hpp>
#include <boost/cobalt/this_coro.hpp>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace AsyncNats {
namespace {

constexpr std::int64_t kRequestTimeoutMs = 30'000;
constexpr std::int64_t kShutdownFlushMs = 1'000;

auto checkedLength(std::string_view value) -> int {
  if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw Error("NATS payload is too large");
  }
  return static_cast<int>(value.size());
}

void requireSubject(std::string_view subject) {
  if (subject.empty() || subject.find_first_of(" \r\n\t") != std::string_view::npos) {
    throw Error("invalid subject");
  }
}

void requireQueue(std::string_view group) {
  if (group.empty() || group.find_first_of(" \r\n\t") != std::string_view::npos) {
    throw Error("invalid queue group");
  }
}

auto statusText(natsStatus status) -> std::string {
  const auto* text = natsStatus_GetText(status);
  return text == nullptr ? "unknown NATS error" : text;
}

void requireStatus(natsStatus status, std::string_view operation,
                   ErrorKind kind = ErrorKind::other) {
  if (status != NATS_OK) {
    throw Error(std::string(operation) + ": " + statusText(status), kind);
  }
}

template <class T>
struct BlockingResult {
  using Storage = std::conditional_t<std::is_void_v<T>, std::monostate, T>;
  std::optional<Storage> value;
  std::exception_ptr failure;
  boost::asio::any_io_executor executor;
};

template <class T, class Function>
struct BlockingAwaiter {
  std::shared_ptr<BlockingResult<T>> result;
  Function function;

  auto await_ready() const noexcept -> bool { return false; }

  void await_suspend(std::coroutine_handle<> coroutine) {
    auto state = result;
    auto operation = std::move(function);
    boost::asio::post(
        Core::instance().threadPool().get_executor(),
        [state = std::move(state), operation = std::move(operation), coroutine]() mutable {
          try {
            if constexpr (std::is_void_v<T>) {
              operation();
              state->value.emplace();
            } else {
              state->value.emplace(operation());
            }
          } catch (...) {
            state->failure = std::current_exception();
          }
          boost::asio::post(state->executor, [coroutine] { coroutine.resume(); });
        });
  }

  auto await_resume() -> T {
    if (result->failure) {
      std::rethrow_exception(result->failure);
    }
    if constexpr (std::is_void_v<T>) {
      return;
    } else {
      return std::move(*result->value);
    }
  }
};

template <class Function>
auto runBlocking(boost::asio::any_io_executor executor, Function function)
    -> boost::cobalt::task<std::invoke_result_t<Function>> {
  using Result = std::invoke_result_t<Function>;
  auto state = std::make_shared<BlockingResult<Result>>();
  state->executor = std::move(executor);
  if constexpr (std::is_void_v<Result>) {
    co_await BlockingAwaiter<Result, Function>{std::move(state), std::move(function)};
    co_return;
  } else {
    co_return co_await BlockingAwaiter<Result, Function>{std::move(state), std::move(function)};
  }
}

auto copyMessage(natsMsg* native) -> Message {
  Message result;
  if (const auto* subject = natsMsg_GetSubject(native)) {
    result.subject = subject;
  }
  if (const auto* reply = natsMsg_GetReply(native); reply != nullptr && *reply != '\0') {
    result.reply = reply;
  }
  const auto* data = natsMsg_GetData(native);
  const auto length = natsMsg_GetDataLength(native);
  if (data != nullptr && length > 0) {
    result.payload.assign(data, static_cast<std::size_t>(length));
  }
  result.noResponders = natsMsg_IsNoResponders(native);
  return result;
}

void logSubscribed(std::string_view subject, const std::optional<std::string>& queue) {
  if (queue.has_value()) {
    spdlog::info("queued topic {} on queue {}", subject, *queue);
    return;
  }
  spdlog::info("subscribed to topic {}", subject);
}

void logUnsubscribed(std::string_view subject, const std::optional<std::string>& queue) {
  if (queue.has_value()) {
    spdlog::info("unsubscribed from topic {} on queue {}", subject, *queue);
    return;
  }
  spdlog::info("unsubscribed from topic {}", subject);
}

}  // namespace

struct Client::Impl : std::enable_shared_from_this<Client::Impl> {
  struct CallbackContext {
    std::weak_ptr<Impl> owner;
    boost::asio::any_io_executor executor;
  };

  struct Waiter {
    std::coroutine_handle<> coroutine;
    Message* result = nullptr;
    bool* failed = nullptr;
    std::string* reason = nullptr;
    ErrorKind* kind = nullptr;
  };

  struct SubState : std::enable_shared_from_this<SubState> {
    std::weak_ptr<Impl> connection;
    std::atomic<natsSubscription*> native = nullptr;
    MessageHandler handler;
    std::deque<Message> messages;
    std::optional<Waiter> waiter;
    bool open = true;
    std::string subject;
    std::optional<std::string> queue;
  };

  natsConnection* native = nullptr;
  std::mutex nativeMutex;
  std::condition_variable nativeDrained;
  std::size_t nativeCalls = 0;
  std::shared_ptr<CallbackContext> callbacks;
  boost::asio::any_io_executor executor;
  std::unordered_map<std::string, std::vector<std::shared_ptr<SubState>>> routes;
  std::unordered_map<std::string,
                     std::unordered_map<std::string, std::vector<std::shared_ptr<SubState>>>>
      queues;
  std::vector<std::shared_ptr<SubState>> subscriptions;
  std::vector<std::coroutine_handle<>> closeWaiters;
  bool failed = false;
  std::string failReason;
  ErrorKind failKind = ErrorKind::interrupted;

  struct Registry {
    ErrorHandler handler;
    std::vector<std::shared_ptr<Impl>> clients;
    bool stopping = false;
    bool inHandler = false;
  };

  struct NativeLease {
    Impl& owner;
    natsConnection* connection;

    explicit NativeLease(Impl& value) : owner(value), connection(nullptr) {
      std::lock_guard lock(owner.nativeMutex);
      if (owner.native == nullptr) {
        throw Error("client is not connected");
      }
      connection = owner.native;
      ++owner.nativeCalls;
    }

    NativeLease(const NativeLease&) = delete;
    auto operator=(const NativeLease&) -> NativeLease& = delete;

    ~NativeLease() {
      std::lock_guard lock(owner.nativeMutex);
      if (--owner.nativeCalls == 0) {
        owner.nativeDrained.notify_all();
      }
    }
  };

  static auto sessions() -> Registry& {
    static Registry value;
    return value;
  }

  ~Impl() {
    if (native != nullptr) {
      natsConnection_Close(native);
      natsConnection_Destroy(native);
      native = nullptr;
    }
  }

  void failOnLoop(std::string reason, ErrorKind kind) {
    if (failed) {
      return;
    }
    failed = true;
    failReason = std::move(reason);
    failKind = kind;

    std::vector<std::coroutine_handle<>> wake;
    for (auto& sub : subscriptions) {
      sub->open = false;
      if (sub->waiter) {
        *sub->waiter->failed = true;
        *sub->waiter->reason = failReason;
        *sub->waiter->kind = failKind;
        wake.push_back(sub->waiter->coroutine);
        sub->waiter.reset();
      }
    }
    auto closing = std::move(closeWaiters);
    closeWaiters.clear();
    wake.insert(wake.end(), closing.begin(), closing.end());
    for (auto coroutine : wake) {
      boost::asio::post(executor, [coroutine] { coroutine.resume(); });
    }
  }

  void deliver(const std::shared_ptr<SubState>& sub, Message message) {
    if (!sub->open) {
      return;
    }
    if (sub->handler) {
      try {
        auto task = sub->handler(std::move(message));
        boost::cobalt::spawn(Core::instance().threadPool().get_executor(), std::move(task),
                             [](std::exception_ptr exception) {
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

    if (sub->waiter) {
      *sub->waiter->result = std::move(message);
      const auto coroutine = sub->waiter->coroutine;
      sub->waiter.reset();
      boost::asio::post(executor, [coroutine] { coroutine.resume(); });
      return;
    }
    sub->messages.push_back(std::move(message));
  }

  static void onMessage(natsConnection*, natsSubscription*, natsMsg* message, void* closure) {
    auto* raw = static_cast<SubState*>(closure);
    if (message == nullptr) {
      return;
    }
    Message copy;
    try {
      copy = copyMessage(message);
    } catch (...) {
      natsMsg_Destroy(message);
      return;
    }
    natsMsg_Destroy(message);

    try {
      auto sub = raw->shared_from_this();
      if (auto owner = sub->connection.lock()) {
        boost::asio::post(owner->executor, [owner = std::move(owner), sub = std::move(sub),
                                            copy = std::move(copy)]() mutable {
          owner->deliver(sub, std::move(copy));
        });
      }
    } catch (...) {
      // The connection is shutting down; the copied message can be discarded.
    }
  }

  static void onClosed(natsConnection*, void* closure) {
    auto* context = static_cast<CallbackContext*>(closure);
    auto weak = context->owner;
    boost::asio::post(context->executor, [weak] {
      if (auto owner = weak.lock()) {
        if (owner->failed) {
          return;
        }
        owner->failOnLoop("NATS connection closed", ErrorKind::interrupted);
        boost::cobalt::spawn(owner->executor,
                             reportError(Error("NATS connection closed", ErrorKind::interrupted)),
                             [](std::exception_ptr) {});
      }
    });
  }

  static void onAsyncError(natsConnection*, natsSubscription*, natsStatus status, void* closure) {
    auto* context = static_cast<CallbackContext*>(closure);
    auto executor = context->executor;
    boost::asio::post(executor, [executor = std::move(executor), status] {
      const auto error = Error("NATS asynchronous error: " + statusText(status));
      boost::cobalt::spawn(executor, reportError(error), [](std::exception_ptr) {});
    });
  }

  auto openSubscription(const std::shared_ptr<SubState>& subscription,
                        std::optional<std::string_view> queue) -> boost::cobalt::task<void> {
    auto lease = std::make_shared<NativeLease>(*this);
    auto* callbackState = subscription.get();
    const auto subject = subscription->subject;
    const auto group = std::string(queue.value_or(std::string_view{}));
    const auto queued = queue.has_value();
    co_await runBlocking(
        executor, [lease = std::move(lease), subject, group, queued, callbackState] {
          natsSubscription* result = nullptr;
          const auto status =
              queued ? natsConnection_QueueSubscribe(&result, lease->connection, subject.c_str(),
                                                     group.c_str(), onMessage, callbackState)
                     : natsConnection_Subscribe(&result, lease->connection, subject.c_str(),
                                                onMessage, callbackState);
          if (status != NATS_OK) {
            if (result != nullptr) {
              natsSubscription_Destroy(result);
            }
            requireStatus(status, queued ? "queue" : "subscribe");
          }
          callbackState->native.store(result);
        });
  }

  auto nextMessage(const std::shared_ptr<SubState>& sub,
                   std::optional<std::chrono::milliseconds> timeout = std::nullopt)
      -> boost::cobalt::task<std::optional<Message>> {
    co_await boost::asio::dispatch(executor, boost::cobalt::use_op);
    if (!sub->messages.empty()) {
      auto result = std::move(sub->messages.front());
      sub->messages.pop_front();
      co_return result;
    }
    if (failed) {
      throw Error(failReason, failKind);
    }
    if (!sub->open) {
      throw Error("subscription closed");
    }

    struct Pending {
      std::shared_ptr<SubState> sub;
      Message result;
      bool failed = false;
      bool timedOut = false;
      bool settled = false;
      std::string reason;
      ErrorKind kind = ErrorKind::other;
      std::optional<boost::asio::steady_timer> timer;
      boost::asio::any_io_executor executor;
    };
    auto pending = std::make_shared<Pending>();
    pending->sub = sub;
    pending->executor = executor;

    struct Wait {
      std::shared_ptr<Pending> pending;
      std::optional<std::chrono::milliseconds> timeout;

      auto await_ready() const noexcept -> bool { return false; }
      void await_suspend(std::coroutine_handle<> coroutine) {
        pending->sub->waiter =
            Waiter{coroutine, &pending->result, &pending->failed, &pending->reason, &pending->kind};
        if (!timeout) {
          return;
        }
        pending->timer.emplace(pending->executor);
        pending->timer->expires_after(*timeout);
        auto state = pending;
        pending->timer->async_wait([state](const boost::system::error_code& ec) {
          if (ec || state->settled || !state->sub->waiter) {
            return;
          }
          state->settled = true;
          state->timedOut = true;
          auto resume = state->sub->waiter->coroutine;
          state->sub->waiter.reset();
          boost::asio::post(state->executor, [resume] { resume.resume(); });
        });
      }
      void await_resume() const {
        pending->settled = true;
        if (pending->timer) {
          pending->timer->cancel();
        }
        if (pending->failed) {
          throw Error(pending->reason.empty() ? "subscription closed" : pending->reason,
                      pending->kind);
        }
      }
    };

    co_await Wait{pending, timeout};
    if (pending->timedOut) {
      co_return std::nullopt;
    }
    co_return std::move(pending->result);
  }

  auto receive(const std::shared_ptr<SubState>& sub,
               std::optional<std::chrono::milliseconds> timeout = std::nullopt,
               std::string timeoutError = {}) -> boost::cobalt::task<Message> {
    auto message = co_await nextMessage(sub, timeout);
    if (!message) {
      throw Error(timeoutError.empty() ? "request: Timeout" : std::move(timeoutError));
    }
    co_return std::move(*message);
  }

  auto unsubscribe(const std::shared_ptr<SubState>& sub) -> boost::cobalt::task<void> {
    co_await boost::asio::dispatch(executor, boost::cobalt::use_op);
    if (!sub->open) {
      co_return;
    }
    sub->open = false;
    if (auto* nativeSub = sub->native.exchange(nullptr)) {
      auto lease = std::make_shared<NativeLease>(*this);
      co_await runBlocking(executor, [lease = std::move(lease), nativeSub] {
        const auto status = natsSubscription_Unsubscribe(nativeSub);
        natsSubscription_Destroy(nativeSub);
        requireStatus(status, "unsubscribe");
      });
    }
    if (sub->waiter) {
      *sub->waiter->failed = true;
      *sub->waiter->reason = "subscription closed";
      *sub->waiter->kind = ErrorKind::closed;
      auto coroutine = sub->waiter->coroutine;
      sub->waiter.reset();
      boost::asio::post(executor, [coroutine] { coroutine.resume(); });
    }
    sub->messages.clear();
  }

  auto shutdown(std::string reason, ErrorKind kind) -> boost::cobalt::task<void> {
    co_await boost::asio::dispatch(executor, boost::cobalt::use_op);
    if (!failed) {
      failOnLoop(std::move(reason), kind);
    }
    natsConnection* connection = nullptr;
    if (native != nullptr) {
      std::lock_guard lock(nativeMutex);
      connection = native;
      native = nullptr;
    }
    if (connection != nullptr) {
      for (const auto& sub : subscriptions) {
        if (sub->native.load() != nullptr) {
          logUnsubscribed(sub->subject, sub->queue);
        }
      }
      spdlog::default_logger()->flush();
      auto self = shared_from_this();
      auto subs = subscriptions;
      co_await runBlocking(executor, [self = std::move(self), connection, subs = std::move(subs)] {
        {
          std::unique_lock lock(self->nativeMutex);
          self->nativeDrained.wait(lock, [&] { return self->nativeCalls == 0; });
        }
        for (auto& sub : subs) {
          auto* nativeSub = sub->native.exchange(nullptr);
          if (nativeSub == nullptr) {
            continue;
          }
          const auto status = natsSubscription_Unsubscribe(nativeSub);
          if (status != NATS_OK) {
            spdlog::error("unsubscribe: {}", statusText(status));
            spdlog::default_logger()->flush();
          }
          natsSubscription_Destroy(nativeSub);
        }
        const auto flushed = natsConnection_FlushTimeout(connection, kShutdownFlushMs);
        if (flushed != NATS_OK && flushed != NATS_CONNECTION_CLOSED) {
          spdlog::error("unsubscribe: {}", statusText(flushed));
          spdlog::default_logger()->flush();
        }
        natsConnection_Close(connection);
        natsConnection_Destroy(connection);
      });
    }
    for (auto& sub : subscriptions) {
      sub->handler = nullptr;
      sub->messages.clear();
      sub->open = false;
    }
    routes.clear();
    queues.clear();
    subscriptions.clear();
    std::erase_if(sessions().clients, [this](const auto& client) { return client.get() == this; });
  }

  static void track(const std::shared_ptr<Impl>& client) {
    auto& registry = sessions();
    registry.clients.push_back(client);
  }

  static auto closeAll(const Error& failure) -> boost::cobalt::task<void> {
    std::vector<std::shared_ptr<Impl>> clients;
    clients = std::move(sessions().clients);
    sessions().clients.clear();
    for (auto& client : clients) {
      co_await client->shutdown(failure.what(), failure.kind());
    }
  }
};

auto reportError(const Error& failure) -> boost::cobalt::task<void> {
  if (failure.kind() == ErrorKind::signal) {
    spdlog::info("{}", failure.what());
  } else if (failure.kind() != ErrorKind::closed) {
    spdlog::error("{}", failure.what());
  }
  auto& registry = Client::Impl::sessions();
  auto handler = registry.handler;
  if (!handler || registry.inHandler) {
    co_return;
  }
  registry.inHandler = true;
  struct ResetFlag {
    bool& value;
    ~ResetFlag() { value = false; }
  } reset{registry.inHandler};
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
  std::shared_ptr<Client::Impl::SubState> subscription;
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
  auto callerExecutor = co_await boost::cobalt::this_coro::executor;
  auto state = std::make_shared<Client::Impl>();
  state->executor = AsyncNatsEventLoop::executor();
  state->callbacks = std::make_shared<Client::Impl::CallbackContext>();
  state->callbacks->owner = state;
  state->callbacks->executor = state->executor;

  natsConnection* native = nullptr;
  std::optional<Error> connectFailure;
  try {
    native = co_await runBlocking(callerExecutor, [url = std::move(url), state]() mutable {
      natsOptions* options = nullptr;
      requireStatus(natsOptions_Create(&options), "create NATS options", ErrorKind::unreachable);
      struct OptionsGuard {
        natsOptions* value;
        ~OptionsGuard() { natsOptions_Destroy(value); }
      } guard{options};

      requireStatus(natsOptions_SetURL(options, url.c_str()), "set NATS URL",
                    ErrorKind::unreachable);
      requireStatus(AsyncNatsEventLoop::configure(options), "set NATS event loop",
                    ErrorKind::unreachable);
      requireStatus(
          natsOptions_SetClosedCB(options, Client::Impl::onClosed, state->callbacks.get()),
          "set NATS closed callback", ErrorKind::unreachable);
      requireStatus(
          natsOptions_SetErrorHandler(options, Client::Impl::onAsyncError, state->callbacks.get()),
          "set NATS error callback", ErrorKind::unreachable);

      natsConnection* connection = nullptr;
      const auto status = natsConnection_Connect(&connection, options);
      if (status != NATS_OK) {
        throw Error("NATS connection failed: " + statusText(status), ErrorKind::unreachable);
      }
      return connection;
    });
  } catch (const Error& failure) {
    connectFailure = failure;
  } catch (const std::exception& failure) {
    connectFailure.emplace("NATS connection failed: " + std::string(failure.what()),
                           ErrorKind::unreachable);
  }
  if (connectFailure) {
    co_await reportError(*connectFailure);
    throw *connectFailure;
  }

  co_await boost::asio::dispatch(state->executor, boost::cobalt::use_op);
  state->native = native;
  if (stopRequested()) {
    co_await state->shutdown("received signal, closing NATS client", ErrorKind::signal);
    throw Error("received signal, closing NATS client", ErrorKind::signal);
  }
  Client::Impl::track(state);
  co_return Client{std::move(state)};
}

auto Client::publish(std::string subject, std::string payload) -> boost::cobalt::task<void> {
  if (!impl_) {
    throw Error("client is not connected");
  }
  requireSubject(subject);
  const auto payloadLength = checkedLength(payload);
  auto state = impl_;
  co_await boost::asio::dispatch(state->executor, boost::cobalt::use_op);
  auto lease = std::make_shared<Impl::NativeLease>(*state);
  co_await runBlocking(state->executor, [lease = std::move(lease), subject = std::move(subject),
                                         payload = std::move(payload), payloadLength] {
    requireStatus(
        natsConnection_Publish(lease->connection, subject.c_str(), payload.data(), payloadLength),
        "publish");
  });
}

auto Client::subscribe(std::string subject) -> boost::cobalt::task<Subscription> {
  if (!impl_) {
    throw Error("client is not connected");
  }
  requireSubject(subject);
  co_await boost::asio::dispatch(impl_->executor, boost::cobalt::use_op);

  auto subscription = std::make_shared<Impl::SubState>();
  subscription->connection = impl_;
  subscription->subject = std::move(subject);
  impl_->subscriptions.push_back(subscription);
  co_await impl_->openSubscription(subscription, std::nullopt);
  logSubscribed(subscription->subject, std::nullopt);
  auto state = std::make_shared<Subscription::State>();
  state->connection = impl_;
  state->subscription = std::move(subscription);
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
  co_await boost::asio::dispatch(impl_->executor, boost::cobalt::use_op);
  std::vector<std::shared_ptr<Impl::SubState>> added;
  std::exception_ptr failure;
  try {
    for (auto& route : routes) {
      auto subscription = std::make_shared<Impl::SubState>();
      subscription->connection = impl_;
      subscription->subject = route.subject;
      subscription->handler = std::move(route.handler);
      impl_->subscriptions.push_back(subscription);
      added.push_back(subscription);
      co_await impl_->openSubscription(subscription, std::nullopt);
      impl_->routes[subscription->subject].push_back(subscription);
    }
  } catch (...) {
    failure = std::current_exception();
  }
  if (failure) {
    for (auto& subscription : added) {
      if (auto found = impl_->routes.find(subscription->subject); found != impl_->routes.end()) {
        std::erase(found->second, subscription);
        if (found->second.empty()) {
          impl_->routes.erase(found);
        }
      }
      try {
        co_await impl_->unsubscribe(subscription);
      } catch (...) {
      }
    }
    std::rethrow_exception(failure);
  }
  for (const auto& subscription : added) {
    logSubscribed(subscription->subject, std::nullopt);
  }
}

auto Client::unsubscribe(std::vector<std::string> subjects) -> boost::cobalt::task<void> {
  if (!impl_) {
    throw Error("client is not connected");
  }
  for (const auto& subject : subjects) {
    requireSubject(subject);
  }
  co_await boost::asio::dispatch(impl_->executor, boost::cobalt::use_op);
  for (const auto& subject : subjects) {
    auto found = impl_->routes.find(subject);
    if (found == impl_->routes.end()) {
      continue;
    }
    auto subscriptions = std::move(found->second);
    impl_->routes.erase(found);
    for (auto& subscription : subscriptions) {
      co_await impl_->unsubscribe(subscription);
    }
    logUnsubscribed(subject, std::nullopt);
  }
}

auto Client::queue(std::string group, std::string subject) -> boost::cobalt::task<Subscription> {
  if (!impl_) {
    throw Error("client is not connected");
  }
  requireQueue(group);
  requireSubject(subject);
  co_await boost::asio::dispatch(impl_->executor, boost::cobalt::use_op);

  auto subscription = std::make_shared<Impl::SubState>();
  subscription->connection = impl_;
  subscription->subject = std::move(subject);
  subscription->queue = group;
  impl_->subscriptions.push_back(subscription);
  co_await impl_->openSubscription(subscription, group);
  logSubscribed(subscription->subject, subscription->queue);
  auto state = std::make_shared<Subscription::State>();
  state->connection = impl_;
  state->subscription = std::move(subscription);
  co_return Subscription{std::move(state)};
}

auto Client::queue(std::string group, Route route) -> boost::cobalt::task<void> {
  std::vector<Route> routes;
  routes.push_back(std::move(route));
  co_await queue(std::move(group), std::move(routes));
}

auto Client::queue(std::string group, std::vector<Route> routes) -> boost::cobalt::task<void> {
  if (!impl_) {
    throw Error("client is not connected");
  }
  requireQueue(group);
  for (const auto& route : routes) {
    requireSubject(route.subject);
    if (!route.handler) {
      throw Error("invalid handler");
    }
  }
  if (routes.empty()) {
    co_return;
  }
  co_await boost::asio::dispatch(impl_->executor, boost::cobalt::use_op);
  std::vector<std::shared_ptr<Impl::SubState>> added;
  std::exception_ptr failure;
  try {
    for (auto& route : routes) {
      auto subscription = std::make_shared<Impl::SubState>();
      subscription->connection = impl_;
      subscription->subject = route.subject;
      subscription->queue = group;
      subscription->handler = std::move(route.handler);
      impl_->subscriptions.push_back(subscription);
      added.push_back(subscription);
      co_await impl_->openSubscription(subscription, group);
      impl_->queues[group][subscription->subject].push_back(subscription);
    }
  } catch (...) {
    failure = std::current_exception();
  }
  if (failure) {
    for (auto& subscription : added) {
      if (auto found = impl_->queues.find(group); found != impl_->queues.end()) {
        if (auto subjects = found->second.find(subscription->subject);
            subjects != found->second.end()) {
          std::erase(subjects->second, subscription);
          if (subjects->second.empty()) {
            found->second.erase(subjects);
          }
        }
        if (found->second.empty()) {
          impl_->queues.erase(found);
        }
      }
      try {
        co_await impl_->unsubscribe(subscription);
      } catch (...) {
      }
    }
    std::rethrow_exception(failure);
  }
  for (const auto& subscription : added) {
    logSubscribed(subscription->subject, subscription->queue);
  }
}

auto Client::unsubscribe(std::string group, std::vector<std::string> subjects)
    -> boost::cobalt::task<void> {
  if (!impl_) {
    throw Error("client is not connected");
  }
  requireQueue(group);
  for (const auto& subject : subjects) {
    requireSubject(subject);
  }
  co_await boost::asio::dispatch(impl_->executor, boost::cobalt::use_op);
  auto found = impl_->queues.find(group);
  if (found == impl_->queues.end()) {
    co_return;
  }
  std::vector<std::shared_ptr<Impl::SubState>> dropping;
  std::vector<std::string> dropped;
  for (const auto& subject : subjects) {
    auto subjectsFound = found->second.find(subject);
    if (subjectsFound == found->second.end()) {
      continue;
    }
    dropping.insert(dropping.end(), std::make_move_iterator(subjectsFound->second.begin()),
                    std::make_move_iterator(subjectsFound->second.end()));
    found->second.erase(subjectsFound);
    dropped.push_back(subject);
  }
  if (found->second.empty()) {
    impl_->queues.erase(found);
  }
  for (auto& subscription : dropping) {
    co_await impl_->unsubscribe(subscription);
  }
  for (const auto& subject : dropped) {
    logUnsubscribed(subject, group);
  }
}

auto Client::disconnect() -> boost::cobalt::task<void> {
  if (!impl_) {
    co_return;
  }
  co_await impl_->shutdown("connection closed", ErrorKind::closed);
}

auto Client::close() -> boost::cobalt::task<void> { co_await disconnect(); }

auto Client::closed() -> boost::cobalt::task<void> {
  if (!impl_) {
    throw Error("client is not connected");
  }
  co_await boost::asio::dispatch(impl_->executor, boost::cobalt::use_op);
  if (impl_->failed) {
    throw Error(impl_->failReason, impl_->failKind);
  }
  struct Wait {
    Impl* self;
    auto await_ready() const noexcept -> bool { return false; }
    void await_suspend(std::coroutine_handle<> coroutine) {
      self->closeWaiters.push_back(coroutine);
    }
    void await_resume() const noexcept {}
  };
  co_await Wait{impl_.get()};
  throw Error(impl_->failReason, impl_->failKind);
}

auto Subscription::next() -> boost::cobalt::task<Message> {
  if (!state_ || !state_->connection || !state_->subscription) {
    throw Error("subscription is closed");
  }
  co_return co_await state_->connection->receive(state_->subscription);
}

auto Subscription::unsubscribe() -> boost::cobalt::task<void> {
  if (!state_ || !state_->connection || !state_->subscription) {
    co_return;
  }
  const auto subject = state_->subscription->subject;
  const auto queue = state_->subscription->queue;
  co_await state_->connection->unsubscribe(state_->subscription);
  logUnsubscribed(subject, queue);
  state_->connection.reset();
  state_->subscription.reset();
}

auto Client::request(std::string subject, std::string payload) -> boost::cobalt::task<std::string> {
  co_return co_await request(std::move(subject), std::move(payload), {});
}

auto Client::request(std::string subject, std::string payload,
                     std::vector<std::pair<std::string, std::string>> headers)
    -> boost::cobalt::task<std::string> {
  if (!impl_) {
    throw Error("client is not connected");
  }
  requireSubject(subject);
  const auto payloadLength = checkedLength(payload);
  auto state = impl_;
  co_await boost::asio::dispatch(state->executor, boost::cobalt::use_op);

  auto inbox = co_await runBlocking(state->executor, [] {
    natsInbox* created = nullptr;
    requireStatus(natsInbox_Create(&created), "create inbox");
    struct InboxGuard {
      natsInbox* value;
      ~InboxGuard() { natsInbox_Destroy(value); }
    } guard{created};
    return std::string(created);
  });

  auto subscription = std::make_shared<Impl::SubState>();
  subscription->connection = state;
  subscription->subject = inbox;
  state->subscriptions.push_back(subscription);

  std::exception_ptr failure;
  std::string body;
  try {
    co_await state->openSubscription(subscription, std::nullopt);
    auto lease = std::make_shared<Impl::NativeLease>(*state);
    co_await runBlocking(state->executor, [lease = std::move(lease), subject = std::move(subject),
                                           payload = std::move(payload),
                                           headers = std::move(headers), payloadLength,
                                           inbox]() mutable {
      natsMsg* outbound = nullptr;
      requireStatus(
          natsMsg_Create(&outbound, subject.c_str(), inbox.c_str(), payload.data(), payloadLength),
          "create request message");
      struct MessageGuard {
        natsMsg* value;
        ~MessageGuard() { natsMsg_Destroy(value); }
      } guard{outbound};
      for (const auto& [name, value] : headers) {
        requireStatus(natsMsgHeader_Add(outbound, name.c_str(), value.c_str()),
                      "set request header");
      }
      requireStatus(natsConnection_PublishMsg(lease->connection, outbound), "request");
    });
    auto message =
        co_await state->receive(subscription, std::chrono::milliseconds(kRequestTimeoutMs),
                                "request: " + statusText(NATS_TIMEOUT));
    if (message.noResponders) {
      throw Error("request: " + statusText(NATS_NO_RESPONDERS));
    }
    body = std::move(message.payload);
  } catch (...) {
    failure = std::current_exception();
  }
  try {
    co_await state->unsubscribe(subscription);
  } catch (...) {
    if (!failure) {
      failure = std::current_exception();
    }
  }
  if (failure) {
    std::rethrow_exception(failure);
  }
  co_return body;
}

auto Client::requestMany(std::string subject, std::string payload, std::size_t count)
    -> boost::cobalt::task<std::vector<std::string>> {
  if (count == 0) {
    throw Error("invalid response count");
  }
  co_return co_await requestMany(std::move(subject), std::move(payload), {}, count, std::nullopt);
}

auto Client::requestMany(std::string subject, std::string payload,
                         std::chrono::milliseconds deadline)
    -> boost::cobalt::task<std::vector<std::string>> {
  if (deadline.count() <= 0) {
    throw Error("invalid timeout");
  }
  co_return co_await requestMany(std::move(subject), std::move(payload), {}, std::nullopt,
                                 deadline);
}

auto Client::requestMany(std::string subject, std::string payload,
                         std::vector<std::pair<std::string, std::string>> headers,
                         std::size_t count) -> boost::cobalt::task<std::vector<std::string>> {
  if (count == 0) {
    throw Error("invalid response count");
  }
  co_return co_await requestMany(std::move(subject), std::move(payload), std::move(headers), count,
                                 std::nullopt);
}

auto Client::requestMany(std::string subject, std::string payload,
                         std::vector<std::pair<std::string, std::string>> headers,
                         std::chrono::milliseconds deadline)
    -> boost::cobalt::task<std::vector<std::string>> {
  if (deadline.count() <= 0) {
    throw Error("invalid timeout");
  }
  co_return co_await requestMany(std::move(subject), std::move(payload), std::move(headers),
                                 std::nullopt, deadline);
}

auto Client::requestMany(std::string subject, std::string payload,
                         std::vector<std::pair<std::string, std::string>> headers,
                         std::optional<std::size_t> count,
                         std::optional<std::chrono::milliseconds> deadline)
    -> boost::cobalt::task<std::vector<std::string>> {
  if (!impl_) {
    throw Error("client is not connected");
  }
  requireSubject(subject);
  const auto payloadLength = checkedLength(payload);
  auto state = impl_;
  co_await boost::asio::dispatch(state->executor, boost::cobalt::use_op);

  auto inbox = co_await runBlocking(state->executor, [] {
    natsInbox* created = nullptr;
    requireStatus(natsInbox_Create(&created), "create inbox");
    struct InboxGuard {
      natsInbox* value;
      ~InboxGuard() { natsInbox_Destroy(value); }
    } guard{created};
    return std::string(created);
  });

  auto subscription = std::make_shared<Impl::SubState>();
  subscription->connection = state;
  subscription->subject = inbox;
  state->subscriptions.push_back(subscription);

  std::exception_ptr failure;
  std::vector<std::string> bodies;
  try {
    co_await state->openSubscription(subscription, std::nullopt);
    auto lease = std::make_shared<Impl::NativeLease>(*state);
    co_await runBlocking(state->executor, [lease = std::move(lease), subject = std::move(subject),
                                           payload = std::move(payload),
                                           headers = std::move(headers), payloadLength,
                                           inbox]() mutable {
      natsMsg* outbound = nullptr;
      requireStatus(
          natsMsg_Create(&outbound, subject.c_str(), inbox.c_str(), payload.data(), payloadLength),
          "create request message");
      struct MessageGuard {
        natsMsg* value;
        ~MessageGuard() { natsMsg_Destroy(value); }
      } guard{outbound};
      for (const auto& [name, value] : headers) {
        requireStatus(natsMsgHeader_Add(outbound, name.c_str(), value.c_str()),
                      "set request header");
      }
      requireStatus(natsConnection_PublishMsg(lease->connection, outbound), "requestMany");
    });
    std::optional<std::chrono::steady_clock::time_point> deadlineAt;
    if (deadline) {
      deadlineAt = std::chrono::steady_clock::now() + *deadline;
    }
    while (!count || bodies.size() < *count) {
      std::optional<std::chrono::milliseconds> budget;
      if (deadlineAt) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= *deadlineAt) {
          budget = std::chrono::milliseconds::zero();
        } else {
          budget = std::chrono::duration_cast<std::chrono::milliseconds>(*deadlineAt - now);
          if (budget->count() <= 0) {
            budget = std::chrono::milliseconds(1);
          }
        }
      }
      auto message = co_await state->nextMessage(subscription, budget);
      if (!message) {
        break;
      }
      if (message->noResponders) {
        if (bodies.empty()) {
          throw Error("requestMany: " + statusText(NATS_NO_RESPONDERS));
        }
        break;
      }
      bodies.push_back(std::move(message->payload));
    }
  } catch (...) {
    failure = std::current_exception();
  }
  try {
    co_await state->unsubscribe(subscription);
  } catch (...) {
    if (!failure) {
      failure = std::current_exception();
    }
  }
  if (failure) {
    std::rethrow_exception(failure);
  }
  co_return bodies;
}

auto Client::respond(const Message& message, std::string payload) -> boost::cobalt::task<void> {
  co_await respond(message, std::move(payload), {});
}

auto Client::respond(const Message& message, std::string payload,
                     std::vector<std::pair<std::string, std::string>> headers)
    -> boost::cobalt::task<void> {
  if (!impl_) {
    throw Error("client is not connected");
  }
  if (!message.reply || message.reply->empty()) {
    throw Error("message has no reply subject");
  }
  auto reply = *message.reply;
  requireSubject(reply);
  const auto payloadLength = checkedLength(payload);
  auto state = impl_;
  co_await boost::asio::dispatch(state->executor, boost::cobalt::use_op);
  auto lease = std::make_shared<Impl::NativeLease>(*state);
  co_await runBlocking(state->executor, [lease = std::move(lease), reply = std::move(reply),
                                         payload = std::move(payload), headers = std::move(headers),
                                         payloadLength] {
    natsMsg* outbound = nullptr;
    requireStatus(natsMsg_Create(&outbound, reply.c_str(), nullptr, payload.data(), payloadLength),
                  "create response");
    struct MessageGuard {
      natsMsg* value;
      ~MessageGuard() { natsMsg_Destroy(value); }
    } guard{outbound};
    for (const auto& [name, value] : headers) {
      requireStatus(natsMsgHeader_Add(outbound, name.c_str(), value.c_str()),
                    "set response header");
    }
    requireStatus(natsConnection_PublishMsg(lease->connection, outbound), "respond");
  });
}

}  // namespace AsyncNats
