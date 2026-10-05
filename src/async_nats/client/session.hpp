/**
 * @file session.hpp
 * @brief Private bridge between the event loop and `Client`.
 *
 * These functions are friends of `Client` and are defined in `client.cpp`.
 * They are not installed.
 */
#pragma once

#include <AsyncNats.h>

#include <boost/cobalt/task.hpp>

namespace AsyncNats {

auto reportError(const Error& failure) -> boost::cobalt::task<void>;
auto closeAllClients(const Error& failure) -> boost::cobalt::task<void>;
void requestStop();
auto stopRequested() -> bool;

}  // namespace AsyncNats
