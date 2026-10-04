#ifndef CORE_NATS_EVENT_LOOP_HPP
#define CORE_NATS_EVENT_LOOP_HPP

#include <nats/nats.h>

#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/cobalt.hpp>
#include <string>

#include "core/client/settings.hpp"

namespace core::nats {

class EventLoop {
 public:
  explicit EventLoop(boost::asio::any_io_executor executor);
  ~EventLoop();

  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  boost::cobalt::promise<bool> connect(const core::client::Settings& settings);
  boost::cobalt::promise<bool> publish(const std::string& subject, const std::string& payload);
  void disconnect();

  bool isConnected() const { return isConnected_; }
  bool isInitialized() const { return m_isInitialized; }

 private:
  boost::asio::any_io_executor m_executor;
  boost::asio::io_context m_context;
  natsConnection* conn_{nullptr};
  natsOptions* m_opts{nullptr};
  bool isConnected_{false};
  bool m_isInitialized{false};
};

}  // namespace core::nats

#endif  // CORE_NATS_EVENT_LOOP_HPP
