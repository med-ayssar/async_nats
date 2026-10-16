#include <AsyncNats.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

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

  co_await client.subscribe(std::vector<AsyncNats::Route>{
      {"service.many",
       [&client](AsyncNats::Message message) -> boost::cobalt::task<void> {
         co_await client.respond(message, "north");
       }},
      {"service.many",
       [&client](AsyncNats::Message message) -> boost::cobalt::task<void> {
         co_await client.respond(message, "south");
       }},
  });

  auto byCount = co_await client.requestMany("service.many", "where", 2);
  for (const auto& reply : byCount) {
    spdlog::info("requestMany count replied {}", reply);
  }
  auto byDeadline = co_await client.requestMany("service.many", "where", std::chrono::seconds(1));
  spdlog::info("requestMany deadline returned {} replies", byDeadline.size());

  co_await client.unsubscribe("echo", {"service.echo"});
  co_await client.unsubscribe("workers", {"service.work"});
  co_await client.unsubscribe({"service.many"});

  const auto both = [](const std::vector<std::string>& replies) {
    return replies.size() == 2 && ((replies[0] == "north" && replies[1] == "south") ||
                                   (replies[0] == "south" && replies[1] == "north"));
  };
  const auto workersOk = (first == "one" || first == "two") && (second == "one" || second == "two");
  const auto manyOk = both(byCount) && both(byDeadline);
  if (echo == "ping" && workersOk && manyOk) {
    co_return 0;
  }
  co_return 1;
}
