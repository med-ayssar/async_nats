#include <async_nats/core.h>

#include "thread_count.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstdlib>
#include <thread>

namespace async_nats {
namespace {

constexpr std::size_t kFallbackThreads = 1;
constexpr std::size_t kIoThreads = 1;

}  // namespace

auto max_thread_count() -> std::size_t {
  const auto hardware = std::thread::hardware_concurrency();
  const auto cores = hardware == 0 ? std::size_t{1} : static_cast<std::size_t>(hardware);
  return cores * 2;
}

auto parse_thread_count(const char* text) -> std::size_t {
  if (text == nullptr || *text == '\0') {
    return kFallbackThreads;
  }
  if (*text == '-') {
    spdlog::warn("invalid thread count '{}', using {}", text, kFallbackThreads);
    return kFallbackThreads;
  }

  errno = 0;
  char* end = nullptr;
  const auto parsed = std::strtoull(text, &end, 10);
  if (end == text || *end != '\0' || errno == ERANGE || parsed == 0) {
    spdlog::warn("invalid thread count '{}', using {}", text, kFallbackThreads);
    return kFallbackThreads;
  }
  const auto limit = max_thread_count();
  if (parsed > limit) {
    spdlog::warn("thread count '{}' is above {}, using {}", text, limit, limit);
    return limit;
  }
  return static_cast<std::size_t>(parsed);
}

struct core::state {
  boost::asio::io_context io;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard;
  boost::asio::thread_pool workers;
  std::thread thread;
  std::size_t worker_count = kFallbackThreads;
  bool released = false;
  bool joined = false;

  explicit state(std::size_t worker_threads)
      : guard(boost::asio::make_work_guard(io)),
        workers(worker_threads),
        worker_count(worker_threads) {}
};

core::core(std::size_t worker_threads) : state_(std::make_unique<state>(worker_threads)) {
  spdlog::info("event loop io threads {} worker threads {}", kIoThreads, worker_threads);
  state_->thread = std::thread([this] { state_->io.run(); });
}

core::~core() {
  release();
  state_->io.stop();
  join();
}

auto core::instance() -> core& {
  static core self{parse_thread_count(std::getenv("NATS_EVENT_LOOP_WORKER_THREADS"))};
  return self;
}

auto core::io_threads() const -> std::size_t { return kIoThreads; }

auto core::worker_threads() const -> std::size_t { return state_->worker_count; }

auto core::thread_pool() -> boost::asio::thread_pool& { return state_->workers; }

auto core::io_context() -> boost::asio::io_context& { return state_->io; }

void core::release() {
  if (state_->released) {
    return;
  }
  state_->released = true;
  state_->guard.reset();
}

void core::join() {
  if (state_->joined) {
    return;
  }
  state_->joined = true;
  if (state_->thread.joinable()) {
    state_->thread.join();
  }
}

}  // namespace async_nats
