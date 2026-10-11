#pragma once

#include <nats/nats.h>
#include <boost/asio/any_io_executor.hpp>

namespace AsyncNats {

/** Bridges the NATS C client's socket callbacks to Core's Asio loop. */
class AsyncNatsEventLoop {
 public:
  static auto executor() -> boost::asio::any_io_executor;
  /** Apply the shared Core IO context to a connection's native options. */
  static auto configure(natsOptions* options) -> natsStatus;

 private:
  static auto attach(void** userData, void* loop, natsConnection* connection, natsSock socket) -> natsStatus;
  static auto setRead(void* userData, bool add) -> natsStatus;
  static auto setWrite(void* userData, bool add) -> natsStatus;
  static auto detach(void* userData) -> natsStatus;
};

}  // namespace AsyncNats
