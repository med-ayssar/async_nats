#include "event_loop.hpp"

#include <async_nats.h>
#include <async_nats/core.h>

#include <boost/cobalt/spawn.hpp>
#include <spdlog/spdlog.h>

#include <exception>
#include <future>

namespace async_nats {

struct event_loop::impl {
  core* runtime = nullptr;
};

event_loop::event_loop() : impl_(std::make_unique<impl>()) {}

event_loop::~event_loop() = default;

void event_loop::setup() {
  if (impl_->runtime != nullptr) {
    return;
  }
  impl_->runtime = &core::instance();
}

auto event_loop::run(int argc, char** argv) -> int {
  setup();
  auto& runtime = *impl_->runtime;
  std::promise<void> finished;

  boost::cobalt::spawn(runtime.io_context(), co_main(argc, argv), [&](std::exception_ptr exception, int result) {
    if (exception) {
      try {
        std::rethrow_exception(exception);
      } catch (const std::exception& error) {
        spdlog::error("Exception {}\n", error.what());
      }
    } else {
      spdlog::info("co_main returned {}", result);
    }

    runtime.release();
    finished.set_value();
  });

  finished.get_future().wait();
  runtime.join();
  return 0;
}

}  // namespace async_nats
