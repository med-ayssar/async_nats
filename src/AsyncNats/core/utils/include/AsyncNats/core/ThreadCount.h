/**
 * @file AsyncNats/core/ThreadCount.h
 * @brief Worker-pool size for `Core`.
 *
 * Not installed. `Core::instance()` reads `NATS_EVENT_LOOP_WORKER_THREADS`
 * through `parseThreadCount`.
 */
#pragma once

#include <cstddef>

namespace AsyncNats {

/**
 * @brief Parse a worker-thread count.
 *
 * Empty, zero, negative, and non-numeric text become 1. A value above
 * `maxThreadCount()` is clamped to that limit.
 */
auto parseThreadCount(const char* text) -> std::size_t;

/** @brief Twice the hardware concurrency, and at least 2. */
auto maxThreadCount() -> std::size_t;

}  // namespace AsyncNats
