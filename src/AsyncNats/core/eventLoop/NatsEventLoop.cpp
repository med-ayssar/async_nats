#include <AsyncNats/core/Core.h>
#include <AsyncNats/eventloop/NatsEventLoop.h>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <system_error>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace AsyncNats {
namespace {

auto protocolFor(natsSock socket, boost::system::error_code& error) -> boost::asio::ip::tcp {
  sockaddr_storage address{};
#if defined(_WIN32)
  int length = sizeof(address);
  if (::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    error.assign(WSAGetLastError(), boost::system::system_category());
    return boost::asio::ip::tcp::v4();
  }
#else
  socklen_t length = sizeof(address);
  if (::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    error.assign(errno, boost::system::generic_category());
    return boost::asio::ip::tcp::v4();
  }
#endif
  return address.ss_family == AF_INET6 ? boost::asio::ip::tcp::v6() : boost::asio::ip::tcp::v4();
}

struct SocketState {
  boost::asio::io_context& io;
  boost::asio::ip::tcp::socket socket;
  natsConnection* connection = nullptr;
  natsSock nativeSocket{};
  std::uint64_t generation = 0;
  bool attached = false;
  bool readEnabled = true;
  bool writeEnabled = false;
  bool readArmed = false;
  bool writeArmed = false;
  bool closingSocket = false;
  std::atomic_bool detaching = false;

  SocketState(boost::asio::io_context& context, natsConnection* nc, natsSock fd)
      : io(context), socket(context), connection(nc), nativeSocket(fd) {}

  void armRead() {
    if (!attached || !readEnabled || readArmed || closingSocket || detaching || !socket.is_open()) {
      return;
    }
    readArmed = true;
    const auto current = generation;
    socket.async_wait(boost::asio::ip::tcp::socket::wait_read,
                      [this, current](const boost::system::error_code& error) {
                        if (current != generation) {
                          return;
                        }
                        readArmed = false;
                        if (detaching || closingSocket || !readEnabled) {
                          return;
                        }
                        if (error) {
                          if (error != boost::asio::error::operation_aborted) {
                            natsConnection_ProcessReadEvent(connection);
                          }
                          return;
                        }
                        natsConnection_ProcessReadEvent(connection);
                        armRead();
                      });
  }

  void armWrite() {
    if (!attached || !writeEnabled || writeArmed || closingSocket || detaching || !socket.is_open()) {
      return;
    }
    writeArmed = true;
    const auto current = generation;
    socket.async_wait(boost::asio::ip::tcp::socket::wait_write,
                      [this, current](const boost::system::error_code& error) {
                        if (current != generation) {
                          return;
                        }
                        writeArmed = false;
                        if (detaching || closingSocket || !writeEnabled) {
                          return;
                        }
                        if (error) {
                          if (error != boost::asio::error::operation_aborted) {
                            natsConnection_ProcessWriteEvent(connection);
                          }
                          return;
                        }
                        natsConnection_ProcessWriteEvent(connection);
                        armWrite();
                      });
  }

  void closeSocketForNats() {
    if (closingSocket || !attached) {
      return;
    }
    closingSocket = true;
    ++generation;
    readArmed = false;
    writeArmed = false;
    boost::system::error_code ignored;
    socket.cancel(ignored);
    if (socket.is_open()) {
      (void)socket.release(ignored);
    }
    attached = false;
    natsConnection_ProcessCloseEvent(&nativeSocket);
  }

  void updateRead(bool enabled) {
    readEnabled = enabled;
    if (!enabled && !writeEnabled) {
      closeSocketForNats();
      return;
    }
    if (enabled) {
      armRead();
    }
  }

  void updateWrite(bool enabled) {
    writeEnabled = enabled;
    if (!enabled && !readEnabled) {
      closeSocketForNats();
      return;
    }
    if (enabled) {
      armWrite();
    }
  }

  auto attachSocket(natsConnection* nc, natsSock fd) -> bool {
    if (socket.is_open()) {
      boost::system::error_code ignored;
      socket.cancel(ignored);
      (void)socket.release(ignored);
    }
    connection = nc;
    nativeSocket = fd;
    closingSocket = false;
    detaching = false;
    attached = false;
    readEnabled = true;
    writeEnabled = false;
    readArmed = false;
    writeArmed = false;
    ++generation;
    boost::system::error_code error;
    const auto protocol = protocolFor(fd, error);
    if (error) {
      return false;
    }
    socket.assign(protocol, fd, error);
    if (error) {
      return false;
    }
    attached = true;
    armRead();
    return true;
  }
};

using StateHandle = std::shared_ptr<SocketState>;

auto stateFrom(void* data) -> StateHandle& { return *static_cast<StateHandle*>(data); }

auto sharedIoContext() -> boost::asio::io_context*& {
  static boost::asio::io_context* context = nullptr;
  return context;
}

auto initializationFlag() -> std::once_flag& {
  static std::once_flag flag;
  return flag;
}

auto eventLoopContext() -> boost::asio::io_context& {
  return *sharedIoContext();
}

}  // namespace

void AsyncNatsEventLoop::initialize() {
  std::call_once(initializationFlag(), [] { sharedIoContext() = &Core::instance().ioContext(); });
}

auto AsyncNatsEventLoop::executor() -> boost::asio::any_io_executor {
  initialize();
  return eventLoopContext().get_executor();
}

auto AsyncNatsEventLoop::configure(natsOptions* options) -> natsStatus {
  initialize();
  return natsOptions_SetEventLoop(options, &eventLoopContext(), attach, setRead, setWrite, detach);
}

auto AsyncNatsEventLoop::attach(void** userData, void* loop, natsConnection* connection,
                                natsSock socket) -> natsStatus {
  try {
    auto& io = *static_cast<boost::asio::io_context*>(loop);
    auto* holder = static_cast<StateHandle*>(*userData);
    if (holder == nullptr) {
      holder = new StateHandle(std::make_shared<SocketState>(io, connection, socket));
      *userData = holder;
    }
    auto state = *holder;
    boost::asio::post(io, [state = std::move(state), connection, socket] {
      if (!state->attachSocket(connection, socket)) {
        natsConnection_Close(connection);
      }
    });
    return NATS_OK;
  } catch (...) {
    return NATS_NO_MEMORY;
  }
}

auto AsyncNatsEventLoop::setRead(void* userData, bool add) -> natsStatus {
  try {
    auto state = stateFrom(userData);
    boost::asio::post(state->io, [state, add] { state->updateRead(add); });
    return NATS_OK;
  } catch (...) {
    return NATS_ERR;
  }
}

auto AsyncNatsEventLoop::setWrite(void* userData, bool add) -> natsStatus {
  try {
    auto state = stateFrom(userData);
    boost::asio::post(state->io, [state, add] { state->updateWrite(add); });
    return NATS_OK;
  } catch (...) {
    return NATS_ERR;
  }
}

auto AsyncNatsEventLoop::detach(void* userData) -> natsStatus {
  try {
    auto* holder = static_cast<StateHandle*>(userData);
    auto state = *holder;
    state->detaching.store(true);
    boost::asio::post(state->io, [state = std::move(state), holder] {
      ++state->generation;
      state->readArmed = false;
      state->writeArmed = false;
      boost::system::error_code ignored;
      state->socket.cancel(ignored);
      if (state->socket.is_open()) {
        (void)state->socket.release(ignored);
      }
      state->attached = false;
      natsConnection_ProcessDetachedEvent(state->connection);
      delete holder;
    });
    return NATS_OK;
  } catch (...) {
    return NATS_ERR;
  }
}

}  // namespace AsyncNats
