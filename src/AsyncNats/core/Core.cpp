#include <AsyncNats/core/Core.h>
#include <AsyncNats/core/ThreadCount.h>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <thread>

namespace AsyncNats {
namespace {

constexpr std::size_t kFallbackThreads = 1;
constexpr std::size_t kIoThreads = 1;

}  // namespace

struct Core::State {
  boost::asio::io_context io;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard;
  boost::asio::thread_pool workers;
  std::thread thread;
  std::size_t worker_count = kFallbackThreads;
  bool released = false;
  bool joined = false;

  explicit State(std::size_t workerThreads)
      : guard(boost::asio::make_work_guard(io)),
        workers(workerThreads),
        worker_count(workerThreads) {}
};

Core::Core(std::size_t workerThreads) : state_(std::make_unique<State>(workerThreads)) {
  spdlog::info("event loop io threads {} worker threads {}", kIoThreads, workerThreads);
  state_->thread = std::thread([this] { state_->io.run(); });
}

Core::~Core() {
  release();
  state_->io.stop();
  join();
}

auto Core::instance() -> Core& {
  static Core self{parseThreadCount(std::getenv("NATS_EVENT_LOOP_WORKER_THREADS"))};
  return self;
}

auto Core::ioThreads() const -> std::size_t { return kIoThreads; }

auto Core::workerThreads() const -> std::size_t { return state_->worker_count; }

auto Core::threadPool() -> boost::asio::thread_pool& { return state_->workers; }

auto Core::ioContext() -> boost::asio::io_context& { return state_->io; }

void Core::release() {
  if (state_->released) {
    return;
  }
  state_->released = true;
  state_->guard.reset();
}

void Core::join() {
  if (state_->joined) {
    return;
  }
  state_->joined = true;
  if (state_->thread.joinable()) {
    state_->thread.join();
  }
}

}  // namespace AsyncNats
