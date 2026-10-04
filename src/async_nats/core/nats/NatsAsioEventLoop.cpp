#include "core/nats/NatsAsioEventLoop.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>

namespace core::eventLoop {
// 1. Attach
natsStatus attach(void** userData, void* loop, natsConnection* nc, natsSock fd) {
  auto* ioc = static_cast<asio::io_context*>(loop);

  NatsConnectionData* data = nullptr;
  if (*userData == nullptr) {  // first connect
    data = new NatsConnectionData{ioc, asio::posix::stream_descriptor(*ioc, fd)};
  } else {  // reconnect
    data = static_cast<NatsConnectionData*>(*userData);
    data->socket.release();  // drop old fd
    data->socket = asio::posix::stream_descriptor(*ioc, fd);
  }

  data->nc = nc;
  *userData = data;

  // NATS always wants read interest after attach
  data->start_read();
  return NATS_OK;
}

// 2. Read interest
natsStatus read(void* userData, bool add) {
  auto* data = static_cast<NatsConnectionData*>(userData);
  if (add)
    data->start_read();
  else
    data->stop_read();
  return NATS_OK;
}

// 3. Write interest
natsStatus write(void* userData, bool add) {
  auto* data = static_cast<NatsConnectionData*>(userData);
  if (add)
    data->start_write();
  else
    data->stop_write();
  return NATS_OK;
}

// 4. Detach
natsStatus detach(void* userData) {
  auto* data = static_cast<NatsConnectionData*>(userData);
  data->stop_read();
  data->stop_write();

  // Tell nats the socket can be closed
  natsSock sock = data->socket.release();
  natsConnection_ProcessCloseEvent(&sock);

  // When fully done (sync here)
  natsConnection_ProcessDetachedEvent(data->nc);

  delete data;
  return NATS_OK;
}

void NatsConnectionData::start_read() {
  if (read) return;
  read = true;

  socket.async_wait(asio::posix::stream_descriptor::wait_read, [this](const boost::system::error_code& ec) {
    read = false;
    if (!ec) {
      natsConnection_ProcessReadEvent(nc);
      // NATS may re-add interest → start_read() again
    }
  });
}

void NatsConnectionData::start_write() {
  if (write) return;
  write = true;

  socket.async_wait(asio::posix::stream_descriptor::wait_write, [this](const boost::system::error_code& ec) {
    write = false;
    if (!ec) {
      natsConnection_ProcessWriteEvent(nc);
    }
  });
}

void NatsConnectionData::stop_read() { /* cancel if needed */ read = false; }
void NatsConnectionData::stop_write() { /* cancel if needed */ write = false; }

NatsConnectionData::NatsConnectionData(asio::io_context* ctx, asio::posix::stream_descriptor socket)
    : context(ctx), socket(std::move(socket)) {}

}  // namespace core::eventLoop
