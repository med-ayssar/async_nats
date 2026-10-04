#pragma once

#include <cstddef>

namespace async_nats {

auto parse_thread_count(const char* text) -> std::size_t;
auto max_thread_count() -> std::size_t;

}  // namespace async_nats
