#include <async_nats.h>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/cobalt/spawn.hpp>
#include <spdlog/spdlog.h>

#include <exception>

auto main(int argc, char* argv[]) -> int {
  boost::asio::io_context io;
  auto guard = boost::asio::make_work_guard(io);

  boost::cobalt::spawn(io, co_main(argc, argv), [&](std::exception_ptr ep, int result) {
    if (ep) {
      try {
        std::rethrow_exception(ep);
      } catch (const std::exception& e) {
        spdlog::error("Exception {}\n", e.what());
      }
    } else {
      spdlog::info("co_main returned {}", result);
    }

    guard.reset();
  });

  io.run();
  return 0;
}
