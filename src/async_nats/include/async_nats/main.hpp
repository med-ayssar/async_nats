#pragma once

#include <boost/cobalt/task.hpp>

namespace async_nats {
using main = boost::cobalt::task<int>;
}

auto co_main(int argc, char* argv[]) -> async_nats::main;
