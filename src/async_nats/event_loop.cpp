#include "event_loop.hpp"

#include "session.hpp"

#include <async_nats.h>
#include <async_nats/core.h>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/cobalt/op.hpp>
#include <boost/cobalt/spawn.hpp>
#include <spdlog/spdlog.h>

#include <csignal>
#include <exception>
#include <future>
#include <memory>
#include <string>

namespace async_nats {
namespace {

auto watch_signals(boost::asio::signal_set& signals) -> boost::cobalt::task<void> {
  auto [status, signo] = co_await signals.async_wait(boost::asio::as_tuple(boost::cobalt::use_op));
  if (status) {
    co_return;
  }

  request_stop();
  std::string message = "received signal " + std::to_string(signo) + ", closing NATS client";
  if (signo == SIGINT) {
    message = "received SIGINT, closing NATS client";
  } else if (signo == SIGTERM) {
    message = "received SIGTERM, closing NATS client";
  }
  error failure(std::move(message), error_kind::signal);
  co_await close_all_clients(failure);
  co_await report_error(failure);
}

}  // namespace

struct event_loop::impl {
  core* runtime = nullptr;
  std::unique_ptr<boost::asio::signal_set> signals;
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
  impl_->signals = std::make_unique<boost::asio::signal_set>(runtime.io_context(), SIGINT, SIGTERM);
  std::promise<void> finished;

  boost::cobalt::spawn(runtime.io_context(), watch_signals(*impl_->signals), [](std::exception_ptr exception) {
    if (!exception) {
      return;
    }
    try {
      std::rethrow_exception(exception);
    } catch (const std::exception& failure) {
      spdlog::error("signal handler failed: {}", failure.what());
    }
  });

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

    if (impl_->signals) {
      impl_->signals->cancel();
    }
    runtime.release();
    finished.set_value();
  });

  finished.get_future().wait();
  runtime.join();
  return 0;
}

}  // namespace async_nats
