#pragma once

#include <nats/nats.h>
#include <nats/status.h>

#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/cobalt.hpp>
#include <cstddef>

namespace core::eventLoop {
namespace asio = boost::asio;

natsStatus attach(void** userData, void* loop, natsConnection* nc, natsSock fd);
natsStatus write(void* userData, bool add);
natsStatus read(void* userData, bool add);
natsStatus detach(void* userData);

struct NatsConnectionData {
  NatsConnectionData(asio::io_context* ctx, asio::posix::stream_descriptor sd);

  natsConnection* nc{0};
  asio::io_context* context{0};
  asio::posix::stream_descriptor socket;
  bool read{false};
  bool write{false};

  void start_read();
  void start_write();

  void stop_read();
  void stop_write();
};

}  // namespace core::eventLoop
