#include <coroutine>
#include <iostream>
#include <string>

using namespace std::string_literals;

struct return_type {
  struct promise_type {
    return_type get_return_object() noexcept {
      std::cout << "promise_type::get_return_object: entered\n";
      return return_type{*this};
    }

    void return_void() noexcept { std::cout << "promise_type::return_void: entered\n"; }

    std::suspend_always initial_suspend() noexcept {
      std::cout << "promise_type::initial_suspend: entered\n";
      return {};
    }
    std::suspend_always final_suspend() noexcept {
      std::cout << "promise_type::final_suspend: entered\n";
      return {};
    }
    std::suspend_never yield_value(std::string msg) noexcept {
      std::cout << "promise_type::yield_value: entered\n";
      output_data = std::move(msg);
      return {};
    }

    auto await_transform(std::string) noexcept {
      std::cout << "promise_type::await_transform: entered\n";
      struct awaiter {
        promise_type& promise;
        bool await_ready() const noexcept {
          std::cout << "awaiter::await_ready: entered\n";
          return false;
        }
        std::string await_resume() const noexcept {
          std::cout << "awaiter::await_resume: entered\n";
          return std::move(promise.input_data);
        }
        bool await_suspend(std::coroutine_handle<promise_type>) const noexcept {
          std::cout << "awaiter::await_suspend: entered\n";
          return false;
        }
      };
      return awaiter{*this};
    }
    void unhandled_exception() noexcept {
      std::cout << "promise_type::unhandled_exception: entered\n";
    }
    std::string output_data;
    std::string input_data;
  };
  explicit return_type(promise_type& promise)
      : handle{std::coroutine_handle<promise_type>::from_promise(promise)} {
    std::cout << "return_type::return_type: entered\n";
  }
  ~return_type() noexcept {
    std::cout << "return_type::~return_type: entered\n";
    if (handle) {
      handle.destroy();
    }
  }

  std::string get() {
    std::cout << "return_type::get: entered\n";
    if (not handle.done()) {
      handle.resume();
    }
    return std::move(handle.promise().output_data);
  }

  void put(std::string msg) {
    std::cout << "return_type::put: entered\n";
    handle.promise().input_data = std::move(msg);
    if (not handle.done()) {
      handle.resume();
    }
  }
  std::coroutine_handle<promise_type> handle{};
};

return_type coro_func() {
  std::cout << "coro_func: entered\n";
  co_yield "Hell0 from the coroutine\n"s;
  // std::cout << co_await std::string{};
  std::cout << "coro_func: what\n";
  co_return;
}

int main() {
  std::cout << "main: entered\n";
  auto rt = coro_func();
  std::cout << rt.get() << std::endl;
  // rt.put("Hello Loulopu\n");
  std::cout << "main: exiting\n";
  return 0;
}
