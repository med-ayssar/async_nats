#include "thread_count.hpp"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstdlib>
#include <thread>

namespace AsyncNats {
namespace {

constexpr std::size_t kFallbackThreads = 1;

}  // namespace

auto maxThreadCount() -> std::size_t {
  const auto hardware = std::thread::hardware_concurrency();
  const auto cores = hardware == 0 ? std::size_t{1} : static_cast<std::size_t>(hardware);
  return cores * 2;
}

auto parseThreadCount(const char* text) -> std::size_t {
  if (text == nullptr || *text == '\0') {
    return kFallbackThreads;
  }
  if (*text == '-') {
    spdlog::warn("invalid thread count '{}', using {}", text, kFallbackThreads);
    return kFallbackThreads;
  }

  errno = 0;
  char* end = nullptr;
  const auto parsed = std::strtoull(text, &end, 10);
  if (end == text || *end != '\0' || errno == ERANGE || parsed == 0) {
    spdlog::warn("invalid thread count '{}', using {}", text, kFallbackThreads);
    return kFallbackThreads;
  }
  const auto limit = maxThreadCount();
  if (parsed > limit) {
    spdlog::warn("thread count '{}' is above {}, using {}", text, limit, limit);
    return limit;
  }
  return static_cast<std::size_t>(parsed);
}

}  // namespace AsyncNats
