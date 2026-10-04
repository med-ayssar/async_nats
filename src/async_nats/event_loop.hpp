#pragma once

#include <memory>

namespace async_nats {

class event_loop {
 public:
  event_loop();
  ~event_loop();

  event_loop(const event_loop&) = delete;
  event_loop(event_loop&&) = delete;
  auto operator=(const event_loop&) -> event_loop& = delete;
  auto operator=(event_loop&&) -> event_loop& = delete;

  // Starts the IO thread and the worker pool.
  void setup();
  auto run(int argc, char** argv) -> int;

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

}  // namespace async_nats
