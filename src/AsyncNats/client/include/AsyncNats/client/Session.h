/**
 * @file AsyncNats/client/Session.h
 * @brief Private bridge between the event loop and `Client`.
 *
 * These functions are friends of `Client` and are defined in `Client.cpp`.
 * They are not installed.
 */
#pragma once

#include <AsyncNats/client/AsyncNats.h>

#include <boost/cobalt/task.hpp>

namespace AsyncNats {

/** @brief Log `failure` and run `onError`, except for kind `closed` and a handler already on the stack. */
auto reportError(const Error& failure) -> boost::cobalt::task<void>;

/** @brief Close every tracked client with `failure`. */
auto closeAllClients(const Error& failure) -> boost::cobalt::task<void>;

/** @brief Ask in-progress connects to stop. A later `connect` throws kind `signal`. */
void requestStop();

/** @brief Whether `requestStop` has run. */
auto stopRequested() -> bool;

}  // namespace AsyncNats
