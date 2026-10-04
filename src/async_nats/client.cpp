#include <async_nats.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <boost/cobalt/op.hpp>
#include <boost/cobalt/this_coro.hpp>

#include <coroutine>
#include <deque>
#include <string>
#include <utility>

namespace async_nats {
namespace {

struct endpoint {
  std::string host;
  std::string port;
};

auto parse_url(std::string url) -> endpoint {
  constexpr std::string_view scheme = "nats://";
  if (url.starts_with(scheme)) {
    url.erase(0, scheme.size());
  }
  const auto slash = url.find('/');
  if (slash != std::string::npos) {
    url.resize(slash);
  }
  const auto colon = url.rfind(':');
  if (colon == std::string::npos) {
    return {std::move(url), "4222"};
  }
  return {url.substr(0, colon), url.substr(colon + 1)};
}

}  // namespace

struct client::impl {
  boost::asio::ip::tcp::socket socket;
  std::string buffer;
  bool busy = false;
  std::deque<std::coroutine_handle<>> waiters;
  std::uint64_t next_sid = 1;

  explicit impl(boost::asio::ip::tcp::socket socket) : socket(std::move(socket)) {}

  auto lock() -> boost::cobalt::task<void> {
    if (!busy) {
      busy = true;
      co_return;
    }
    struct wait {
      impl* self = nullptr;
      bool await_ready() const noexcept { return false; }
      void await_suspend(std::coroutine_handle<> handle) { self->waiters.push_back(handle); }
      void await_resume() const noexcept {}
    };
    co_await wait{this};
  }

  void unlock() {
    if (waiters.empty()) {
      busy = false;
      return;
    }
    auto handle = waiters.front();
    waiters.pop_front();
    boost::asio::post(socket.get_executor(), [handle] { handle.resume(); });
  }

  auto read_line() -> boost::cobalt::task<std::string> {
    for (;;) {
      const auto mark = buffer.find("\r\n");
      if (mark != std::string::npos) {
        auto line = buffer.substr(0, mark);
        buffer.erase(0, mark + 2);
        co_return line;
      }
      co_await boost::asio::async_read_until(socket, boost::asio::dynamic_buffer(buffer), "\r\n",
                                              boost::cobalt::use_op);
    }
  }

  auto read_exact(std::size_t size) -> boost::cobalt::task<void> {
    while (buffer.size() < size) {
      co_await boost::asio::async_read_until(socket, boost::asio::dynamic_buffer(buffer), "\r\n",
                                              boost::cobalt::use_op);
      if (buffer.size() < size) {
        co_await boost::asio::async_read(socket, boost::asio::dynamic_buffer(buffer),
                                          boost::asio::transfer_at_least(size - buffer.size()),
                                          boost::cobalt::use_op);
      }
    }
  }

  auto write(std::string bytes) -> boost::cobalt::task<void> {
    co_await boost::asio::async_write(socket, boost::asio::buffer(bytes), boost::cobalt::use_op);
  }

  auto next_message() -> boost::cobalt::task<std::string> {
    for (;;) {
      auto line = co_await read_line();
      if (line == "PING") {
        co_await write("PONG\r\n");
        continue;
      }
      if (line.starts_with("-ERR")) {
        throw error(line);
      }
      if (!line.starts_with("MSG ") && !line.starts_with("HMSG ")) {
        continue;
      }

      std::size_t size = 0;
      const auto last = line.rfind(' ');
      if (last == std::string::npos) {
        throw error("malformed NATS message");
      }
      size = static_cast<std::size_t>(std::stoul(line.substr(last + 1)));
      co_await read_exact(size + 2);
      auto payload = buffer.substr(0, size);
      buffer.erase(0, size + 2);

      if (line.starts_with("HMSG ")) {
        const auto header_end = payload.find("\r\n\r\n");
        if (header_end != std::string::npos) {
          payload.erase(0, header_end + 4);
        }
      }
      co_return payload;
    }
  }
};

client::client(std::shared_ptr<impl> impl) : impl_(std::move(impl)) {}

auto connect(std::string url) -> boost::cobalt::task<client> {
  const auto where = parse_url(std::move(url));
  auto executor = co_await boost::cobalt::this_coro::executor;
  auto resolver = boost::asio::ip::tcp::resolver{executor};
  auto results = co_await resolver.async_resolve(where.host, where.port, boost::cobalt::use_op);
  boost::asio::ip::tcp::socket socket{executor};
  co_await boost::asio::async_connect(socket, results, boost::cobalt::use_op);

  auto state = std::make_shared<client::impl>(std::move(socket));
  const auto info = co_await state->read_line();
  if (!info.starts_with("INFO ")) {
    throw error("NATS server did not send INFO");
  }
  co_await state->write(
      "CONNECT {\"verbose\":false,\"pedantic\":false,\"tls_required\":false,"
      "\"lang\":\"cpp\",\"version\":\"0.1.0\",\"protocol\":1}\r\nPING\r\n");

  for (;;) {
    auto line = co_await state->read_line();
    if (line == "PONG") {
      break;
    }
    if (line == "PING") {
      co_await state->write("PONG\r\n");
      continue;
    }
    if (line.starts_with("-ERR")) {
      throw error(line);
    }
  }

  co_return client{std::move(state)};
}

auto client::publish(std::string subject, std::string payload) -> boost::cobalt::task<void> {
  if (!impl_) {
    throw error("client is not connected");
  }
  co_await impl_->lock();
  try {
    co_await impl_->write("PUB " + subject + " " + std::to_string(payload.size()) + "\r\n" + payload + "\r\n");
  } catch (...) {
    impl_->unlock();
    throw;
  }
  impl_->unlock();
}

auto client::request(std::string subject, std::string payload) -> boost::cobalt::task<std::string> {
  return request(std::move(subject), std::move(payload), {});
}

auto client::request(std::string subject, std::string payload,
                     std::vector<std::pair<std::string, std::string>> headers) -> boost::cobalt::task<std::string> {
  if (!impl_) {
    throw error("client is not connected");
  }
  co_await impl_->lock();
  try {
    const auto sid = impl_->next_sid++;
    const auto inbox = "_INBOX." + std::to_string(sid);
    const auto id = std::to_string(sid);
    std::string frame = "SUB " + inbox + " " + id + "\r\n";
    if (headers.empty()) {
      frame += "PUB " + subject + " " + inbox + " " + std::to_string(payload.size()) + "\r\n" + payload + "\r\n";
    } else {
      std::string block = "NATS/1.0\r\n";
      for (const auto& [name, value] : headers) {
        block += name;
        block += ": ";
        block += value;
        block += "\r\n";
      }
      block += "\r\n";
      const auto total = block.size() + payload.size();
      frame += "HPUB " + subject + " " + inbox + " " + std::to_string(block.size()) + " " + std::to_string(total) +
               "\r\n" + block + payload + "\r\n";
    }
    frame += "UNSUB " + id + " 1\r\n";
    co_await impl_->write(std::move(frame));
    auto response = co_await impl_->next_message();
    impl_->unlock();
    co_return response;
  } catch (...) {
    impl_->unlock();
    throw;
  }
}

}  // namespace async_nats
