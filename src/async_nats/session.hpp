#pragma once

#include <async_nats.h>

#include <boost/cobalt/task.hpp>

namespace async_nats {

auto report_error(const error& failure) -> boost::cobalt::task<void>;
auto close_all_clients(const error& failure) -> boost::cobalt::task<void>;
void request_stop();
auto stop_requested() -> bool;

}  // namespace async_nats
