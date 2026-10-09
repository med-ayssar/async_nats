#include "eventLoop/EventLoop.h"

auto main(int argc, char* argv[]) -> int {
  AsyncNats::EventLoop loop;
  loop.setup();
  return loop.run(argc, argv);
}
