#include <AsyncNats.h>
#include <spdlog/spdlog.h>

#include <cstdlib>

AsyncNats::Main co_main(int, char**) {
  const char* url = std::getenv("NATS_URL");
  auto client = co_await AsyncNats::connect(url != nullptr ? url : "nats://127.0.0.1:4222");

  co_await client.queue("echo",
                        AsyncNats::Route{
                            "service.echo",
                            [&client](AsyncNats::Message message) -> boost::cobalt::task<void> {
                              co_await client.respond(message, message.payload);
                            },
                        });

  co_await client.queue("workers",
                        std::vector<AsyncNats::Route>{
                            {"service.work",
                             [&client](AsyncNats::Message message) -> boost::cobalt::task<void> {
                               spdlog::info("worker one got {}", message.payload);
                               co_await client.respond(message, "one");
                             }},
                            {"service.work",
                             [&client](AsyncNats::Message message) -> boost::cobalt::task<void> {
                               spdlog::info("worker two got {}", message.payload);
                               co_await client.respond(message, "two");
                             }},
                        });

  auto echo = co_await client.request("service.echo", "ping");
  spdlog::info("echo replied {}", echo);

  auto first = co_await client.request("service.work", "a");
  auto second = co_await client.request("service.work", "b");
  spdlog::info("workers replied {} and {}", first, second);

  co_await client.unsubscribe("echo", {"service.echo"});
  co_await client.unsubscribe("workers", {"service.work"});

  const auto workersOk = (first == "one" || first == "two") && (second == "one" || second == "two");
  co_return echo == "ping" && workersOk ? 0 : 1;
}
