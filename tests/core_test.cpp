#include <async_nats.h>
#include <async_nats/core.h>

#include "thread_count.hpp"

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

auto hardware_threads() -> std::size_t {
  const auto reported = std::thread::hardware_concurrency();
  return reported == 0 ? std::size_t{1} : static_cast<std::size_t>(reported);
}

struct env_setup {
  env_setup() {
    const auto workers = std::to_string(hardware_threads() + 1);
    setenv("NATS_EVENT_LOOP_WORKER_THREADS", workers.c_str(), 1);
  }
};

const env_setup kEnv;

}  // namespace

TEST_CASE("thread counts default to one and stay above the hardware count") {
  REQUIRE(async_nats::parse_thread_count(nullptr) == 1);
  REQUIRE(async_nats::parse_thread_count("") == 1);
  REQUIRE(async_nats::parse_thread_count("0") == 1);
  REQUIRE(async_nats::parse_thread_count("-2") == 1);
  REQUIRE(async_nats::parse_thread_count("nope") == 1);
  REQUIRE(async_nats::parse_thread_count("2") == 2);

  const auto hardware = hardware_threads();
  const auto limit = async_nats::max_thread_count();
  REQUIRE(limit > hardware);

  const auto above_hardware = std::to_string(hardware + 1);
  const auto at_limit = std::to_string(limit);
  const auto over_limit = std::to_string(limit + 1);
  REQUIRE(async_nats::parse_thread_count(above_hardware.c_str()) == hardware + 1);
  REQUIRE(async_nats::parse_thread_count(at_limit.c_str()) == limit);
  REQUIRE(async_nats::parse_thread_count(over_limit.c_str()) == limit);
}

TEST_CASE("core is a singleton with one io thread and the worker pool") {
  auto& first = async_nats::core::instance();
  auto& second = async_nats::core::instance();
  REQUIRE(&first == &second);
  REQUIRE(first.io_threads() == 1);
  REQUIRE(first.worker_threads() == hardware_threads() + 1);

  std::promise<int> done;
  auto result = done.get_future();
  boost::asio::post(first.thread_pool(), [&done] { done.set_value(7); });
  REQUIRE(result.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
  REQUIRE(result.get() == 7);
}

TEST_CASE("connect reports a server that is not reachable") {
  boost::asio::io_context io;
  std::optional<async_nats::error_kind> kind;
  bool handler_ran = false;

  async_nats::on_error([&](async_nats::error failure) -> boost::cobalt::task<void> {
    handler_ran = true;
    kind = failure.kind();
    co_return;
  });

  boost::cobalt::spawn(io, async_nats::connect("nats://127.0.0.1:1"),
                       [&](std::exception_ptr exception, async_nats::client) {
                         REQUIRE(exception != nullptr);
                         try {
                           std::rethrow_exception(exception);
                         } catch (const async_nats::error& failure) {
                           REQUIRE(failure.kind() == async_nats::error_kind::unreachable);
                           REQUIRE(std::string(failure.what()).find("is not reachable") != std::string::npos);
                         }
                       });
  io.run();
  REQUIRE(handler_ran);
  REQUIRE(kind == async_nats::error_kind::unreachable);
  async_nats::on_error({});
}

TEST_CASE("connect reports a server that drops the connection") {
  boost::asio::io_context io;
  boost::asio::ip::tcp::acceptor acceptor{io, {boost::asio::ip::tcp::v4(), 0}};
  const auto port = std::to_string(acceptor.local_endpoint().port());
  boost::asio::ip::tcp::socket accepted{io};
  acceptor.async_accept(accepted, [&](const boost::system::error_code&) { accepted.close(); });

  std::optional<async_nats::error_kind> kind;
  async_nats::on_error([&](async_nats::error failure) -> boost::cobalt::task<void> {
    kind = failure.kind();
    co_return;
  });

  boost::cobalt::spawn(io, async_nats::connect("nats://127.0.0.1:" + port),
                       [&](std::exception_ptr exception, async_nats::client) {
                         REQUIRE(exception != nullptr);
                         try {
                           std::rethrow_exception(exception);
                         } catch (const async_nats::error& failure) {
                           REQUIRE(failure.kind() == async_nats::error_kind::interrupted);
                           REQUIRE(std::string(failure.what()).find("was interrupted") != std::string::npos);
                         }
                       });
  io.run();
  REQUIRE(kind == async_nats::error_kind::interrupted);
  async_nats::on_error({});
}
