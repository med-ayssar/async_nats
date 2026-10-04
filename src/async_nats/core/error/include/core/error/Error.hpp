#pragma once
#include <array>
#include <expected>
#include <format>
#include <system_error>

namespace core::error {
enum class Error {
  NatsInitFailed = 0,
};

class ErrorCategory : public std::error_category {
 public:
  const char* name() const noexcept override { return "NatsClientError"; }
  std::string message(int value) const override {
    static constexpr auto msgs = std::to_array({
        std::pair{Error::NatsInitFailed, "Failed to setup the Nats-Asio Event Loop"},
    });

    if (value >= msgs.size()) {
      return std::format("Internal Error: Unkown Error Code '{}'", value);
    }
    auto msg = msgs[value];
    return msg.second;
  }
};

inline ErrorCategory& category() noexcept {
  static ErrorCategory instance;
  return instance;
}

template <typename T, typename E = std::error_code>
using Result = std::expected<T, E>;

inline std::error_code make_error_code(Error err) { return {static_cast<int>(err), category()}; }

template <typename T>
Result<T> unexpected(Error err) {
  return std::unexpected(err);
}
}  // namespace core::error
