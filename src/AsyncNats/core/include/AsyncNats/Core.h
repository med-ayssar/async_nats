/**
 * @file AsyncNats/Core.h
 * @brief Process-wide IO thread and worker pool.
 *
 * `main` starts this runtime before the application's `co_main` runs. Post
 * blocking work onto `threadPool()`. NATS socket IO stays on the single IO
 * thread and is not exposed here.
 */
#pragma once

#include <boost/asio/thread_pool.hpp>

#include <cstddef>
#include <memory>

namespace boost::asio {
class io_context;
}

namespace AsyncNats {

class EventLoop;

/**
 * @brief Process-wide runtime: one IO thread and a worker pool.
 *
 * `instance()` returns the singleton. `ioThreads()` is always 1.
 * `NATS_EVENT_LOOP_WORKER_THREADS` sizes the pool and defaults to 1. The pool
 * accepts at most twice the hardware concurrency, and at least 2. A larger
 * value is clamped.
 *
 * Socket operations are not thread-safe. Post blocking work with
 * `boost::asio::post` on `threadPool()`.
 */
class Core {
 public:
  /** @brief The process-wide runtime. Constructed on the first call. */
  static auto instance() -> Core&;

  /** @brief Always 1. Socket IO runs on that thread. */
  auto ioThreads() const -> std::size_t;

  /** @brief Number of threads in the worker pool. */
  auto workerThreads() const -> std::size_t;

  /**
   * @brief Pool for blocking work.
   *
   * Do not run NATS socket operations on this pool.
   */
  auto threadPool() -> boost::asio::thread_pool&;

 private:
  friend class EventLoop;

  auto ioContext() -> boost::asio::io_context&;
  void release();
  void join();

  explicit Core(std::size_t workerThreads);
  ~Core();
  Core(const Core&) = delete;
  Core(Core&&) = delete;
  auto operator=(const Core&) -> Core& = delete;
  auto operator=(Core&&) -> Core& = delete;

  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace AsyncNats
