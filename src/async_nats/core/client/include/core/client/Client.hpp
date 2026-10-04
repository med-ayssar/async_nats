#pragma once

#include <nats.h>
#include <nats/nats.h>
#include <nats/status.h>

#include <boost/cobalt/promise.hpp>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "core/error/Error.hpp"

namespace core::Nats {

template <typename T>
using Ptr = std::shared_ptr<T>;

template <typename T>
using Result = core::error::Result<T>;

template <typename T>
using AsyncResult = boost::cobalt::promise<core::error::Result<T>>;

using NatsOptionsPtr = Ptr<natsOptions>;

class NatsClient {
 public:
  virtual ~NatsClient();
  virtual AsyncResult<void> publish(std::string_view topic, std::string_view data) const;
  virtual AsyncResult<std::string> subscribe(std::string_view topic) const;
};
// Result<NatsOptionsPtr> init(const client::Settings& config);
Result<Ptr<NatsClient>> connect();
}  // namespace core::Nats
