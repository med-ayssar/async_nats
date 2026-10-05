#include <AsyncNats.h>

#include <spdlog/spdlog.h>

#include <cstdlib>

AsyncNats::Main coMain(int, char**) {
  const char* url = std::getenv("NATS_URL");
  auto client = co_await AsyncNats::connect(url != nullptr ? url : "nats://127.0.0.1:4222");

  constexpr auto subject = "async_nats.example";
  auto subscription = co_await client.subscribe(subject);
  co_await client.publish(subject, "hello");

  auto message = co_await subscription.next();
  spdlog::info("received {} on {}", message.payload, message.subject);
  co_await subscription.unsubscribe();
  co_return message.payload == "hello" ? 0 : 1;
}
