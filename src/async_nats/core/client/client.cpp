#include "core/client/main.hpp"

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
      spdlog::error("co_main returned {}\n", result);
    }

    guard.reset();  // allow io.run() to exit
  });

  io.run();
  return 0;
}
