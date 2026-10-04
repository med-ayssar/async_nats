#include "event_loop.hpp"

auto main(int argc, char* argv[]) -> int {
  async_nats::event_loop loop;
  loop.setup();
  return loop.run(argc, argv);
}
