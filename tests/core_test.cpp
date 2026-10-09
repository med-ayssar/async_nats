#include <AsyncNats.h>
#include <AsyncNats/Core.h>

#include "utils/ThreadCount.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/cobalt/spawn.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <future>
#include <optional>
#include <string>
#include <thread>

namespace {

auto hardwareThreads() -> std::size_t {
  const auto reported = std::thread::hardware_concurrency();
  return reported == 0 ? std::size_t{1} : static_cast<std::size_t>(reported);
}

struct EnvSetup {
  EnvSetup() {
    const auto workers = std::to_string(hardwareThreads() + 1);
    setenv("NATS_EVENT_LOOP_WORKER_THREADS", workers.c_str(), 1);
  }
};

const EnvSetup kEnv;

}  // namespace

TEST_CASE("thread counts default to one and stay above the hardware count") {
  REQUIRE(AsyncNats::parseThreadCount(nullptr) == 1);
  REQUIRE(AsyncNats::parseThreadCount("") == 1);
  REQUIRE(AsyncNats::parseThreadCount("0") == 1);
  REQUIRE(AsyncNats::parseThreadCount("-2") == 1);
  REQUIRE(AsyncNats::parseThreadCount("nope") == 1);
  REQUIRE(AsyncNats::parseThreadCount("2") == 2);

  const auto hardware = hardwareThreads();
  const auto limit = AsyncNats::maxThreadCount();
  REQUIRE(limit > hardware);

  const auto above_hardware = std::to_string(hardware + 1);
  const auto at_limit = std::to_string(limit);
  const auto over_limit = std::to_string(limit + 1);
  REQUIRE(AsyncNats::parseThreadCount(above_hardware.c_str()) == hardware + 1);
  REQUIRE(AsyncNats::parseThreadCount(at_limit.c_str()) == limit);
  REQUIRE(AsyncNats::parseThreadCount(over_limit.c_str()) == limit);
}

TEST_CASE("Core is a singleton with one io thread and the worker pool") {
  auto& first = AsyncNats::Core::instance();
  auto& second = AsyncNats::Core::instance();
  REQUIRE(&first == &second);
  REQUIRE(first.ioThreads() == 1);
  REQUIRE(first.workerThreads() == hardwareThreads() + 1);

  std::promise<int> done;
  auto result = done.get_future();
  boost::asio::post(first.threadPool(), [&done] { done.set_value(7); });
  REQUIRE(result.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
  REQUIRE(result.get() == 7);
}

TEST_CASE("connect reports a server that is not reachable") {
  boost::asio::io_context io;
  std::optional<AsyncNats::ErrorKind> kind;
  bool handler_ran = false;

  AsyncNats::onError([&](AsyncNats::Error failure) -> boost::cobalt::task<void> {
    handler_ran = true;
    kind = failure.kind();
    co_return;
  });

  boost::cobalt::spawn(io, AsyncNats::connect("nats://127.0.0.1:1"),
                       [&](std::exception_ptr exception, AsyncNats::Client) {
                         REQUIRE(exception != nullptr);
                         try {
                           std::rethrow_exception(exception);
                         } catch (const AsyncNats::Error& failure) {
                           REQUIRE(failure.kind() == AsyncNats::ErrorKind::unreachable);
                           REQUIRE(std::string(failure.what()).find("is not reachable") != std::string::npos);
                         }
                       });
  io.run();
  REQUIRE(handler_ran);
  REQUIRE(kind == AsyncNats::ErrorKind::unreachable);
  AsyncNats::onError({});
}

TEST_CASE("connect reports a server that drops the connection") {
  boost::asio::io_context io;
  boost::asio::ip::tcp::acceptor acceptor{io, {boost::asio::ip::tcp::v4(), 0}};
  const auto port = std::to_string(acceptor.local_endpoint().port());
  boost::asio::ip::tcp::socket accepted{io};
  acceptor.async_accept(accepted, [&](const boost::system::error_code&) { accepted.close(); });

  std::optional<AsyncNats::ErrorKind> kind;
  AsyncNats::onError([&](AsyncNats::Error failure) -> boost::cobalt::task<void> {
    kind = failure.kind();
    co_return;
  });

  boost::cobalt::spawn(io, AsyncNats::connect("nats://127.0.0.1:" + port),
                       [&](std::exception_ptr exception, AsyncNats::Client) {
                         REQUIRE(exception != nullptr);
                         try {
                           std::rethrow_exception(exception);
                         } catch (const AsyncNats::Error& failure) {
                           REQUIRE(failure.kind() == AsyncNats::ErrorKind::interrupted);
                           REQUIRE(std::string(failure.what()).find("was interrupted") != std::string::npos);
                         }
                       });
  io.run();
  REQUIRE(kind == AsyncNats::ErrorKind::interrupted);
  AsyncNats::onError({});
}
