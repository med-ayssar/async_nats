#pragma once

#include <memory>

namespace AsyncNats {

class EventLoop {
 public:
  EventLoop();
  ~EventLoop();

  EventLoop(const EventLoop&) = delete;
  EventLoop(EventLoop&&) = delete;
  auto operator=(const EventLoop&) -> EventLoop& = delete;
  auto operator=(EventLoop&&) -> EventLoop& = delete;

  /** @brief Start the IO thread and the worker pool. A second call does nothing. */
  void setup();

  /**
   * @brief Run `co_main` and the signal watcher.
   * @return 0. The integer `co_main` returns is only logged.
   */
  auto run(int argc, char** argv) -> int;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace AsyncNats
