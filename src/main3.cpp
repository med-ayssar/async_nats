#include <boost/cobalt.hpp>
#include <chrono>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;
using namespace boost;

cobalt::generator<int> basic_generator() {
  std::this_thread::sleep_for(1s);
  co_yield 1;

  std::this_thread::sleep_for(1s);
  co_return 0;
}
cobalt::generator<int, int> sqg(int x) {
  while (x != 0) {
    x = co_yield x * x;
  }
  co_return 0;
}

template <typename RT, typename Push>
cobalt::task<void> guard(cobalt::generator<RT, Push>& g, const Push& p) {
  if (not g.ready()) {
    co_return;
  }
  std::cout << co_await g(p);
}
boost::cobalt::main co_main(int argc, char** argv) {
  auto g = sqg(10);
  co_await guard(g, 4);
  co_await guard(g, 12);
  co_await guard(g, 0);
  // std::cout << co_await g(4) << std::endl;
  // std::cout << co_await g(12) << std::endl;
  // std::cout << co_await g(0) << std::endl;
  co_return 0;
}
