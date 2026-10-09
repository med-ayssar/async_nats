#include "EventLoop.h"

#include "Session.h"

#include <AsyncNats.h>
#include <AsyncNats/Core.h>

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

namespace AsyncNats {
namespace {

auto watchSignals(boost::asio::signal_set& signals) -> boost::cobalt::task<void> {
  auto [status, signo] = co_await signals.async_wait(boost::asio::as_tuple(boost::cobalt::use_op));
  if (status) {
    co_return;
  }

  requestStop();
  std::string message = "received signal " + std::to_string(signo) + ", closing NATS client";
  if (signo == SIGINT) {
    message = "received SIGINT, closing NATS client";
  } else if (signo == SIGTERM) {
    message = "received SIGTERM, closing NATS client";
  }
  Error failure(std::move(message), ErrorKind::signal);
  co_await closeAllClients(failure);
  co_await reportError(failure);
}

}  // namespace

struct EventLoop::Impl {
  Core* runtime = nullptr;
  std::unique_ptr<boost::asio::signal_set> signals;
};

EventLoop::EventLoop() : impl_(std::make_unique<Impl>()) {}

EventLoop::~EventLoop() = default;

void EventLoop::setup() {
  if (impl_->runtime != nullptr) {
    return;
  }
  impl_->runtime = &Core::instance();
}

auto EventLoop::run(int argc, char** argv) -> int {
  setup();
  auto& runtime = *impl_->runtime;
  impl_->signals = std::make_unique<boost::asio::signal_set>(runtime.ioContext(), SIGINT, SIGTERM);
  std::promise<void> finished;

  boost::cobalt::spawn(runtime.ioContext(), watchSignals(*impl_->signals), [](std::exception_ptr exception) {
    if (!exception) {
      return;
    }
    try {
      std::rethrow_exception(exception);
    } catch (const std::exception& failure) {
      spdlog::error("signal handler failed: {}", failure.what());
    }
  });

  boost::cobalt::spawn(runtime.ioContext(), co_main(argc, argv), [&](std::exception_ptr exception, int result) {
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

}  // namespace AsyncNats
