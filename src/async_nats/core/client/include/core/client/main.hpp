#pragma once
#include <spdlog/spdlog.h>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/cobalt/spawn.hpp>
#include <boost/cobalt/task.hpp>

namespace nats::client {
using main = boost::cobalt::task<int>;
}  // namespace nats::client

auto co_main(int argc, char* argv[]) -> nats::client::main;  // declaration only
auto main(int argc, char* argv[]) -> int;
