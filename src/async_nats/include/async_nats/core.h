#pragma once

#include <boost/asio/thread_pool.hpp>

#include <cstddef>
#include <memory>

namespace boost::asio {
class io_context;
}

namespace async_nats {

class event_loop;

class core {
 public:
  static auto instance() -> core&;

  auto io_threads() const -> std::size_t;
  auto worker_threads() const -> std::size_t;
  auto thread_pool() -> boost::asio::thread_pool&;

 private:
  friend class event_loop;

  auto io_context() -> boost::asio::io_context&;
  void release();
  void join();

  explicit core(std::size_t worker_threads);
  ~core();
  core(const core&) = delete;
  core(core&&) = delete;
  auto operator=(const core&) -> core& = delete;
  auto operator=(core&&) -> core& = delete;

  struct state;
  std::unique_ptr<state> state_;
};

}  // namespace async_nats
