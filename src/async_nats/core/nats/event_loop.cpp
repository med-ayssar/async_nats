#include "core/nats/event_loop.hpp"

#include <nats/nats.h>
#include <nats/status.h>
#include <spdlog/common.h>
#include <spdlog/spdlog.h>

#include <boost/asio/execution/context.hpp>
#include <sstream>
#include <string_view>

#include "core/nats/NatsAsioEventLoop.hpp"
namespace {

template <typename... Args>
void error(bool& success, spdlog::format_string_t<Args...> fmt, Args&&... args) {
  success = false;
  spdlog::error(fmt, std::forward<Args>(args)...);
}

}  // namespace
namespace core::nats {
EventLoop::EventLoop(boost::asio::any_io_executor executor) : m_executor(std::move(executor)) {
  if (not m_executor) {
    error(m_isInitialized, "[Core::nats] Executor is not valid ");
  }
  natsStatus status = natsOptions_Create(&m_opts);
  if (status != NATS_OK) {
    // spdlog::error("[core::nats] Options creation failed: {}", natsStatus_GetText(status));
    error(m_isInitialized, "[core::nats] Options creation failed: {}", natsStatus_GetText(status));
  }

  auto& context = m_executor.context();

  status = natsOptions_SetEventLoop(m_opts, &context, core::eventLoop::attach, core::eventLoop::read,
                                    core::eventLoop::write, core::eventLoop::detach);
  if (status != NATS_OK) {
    spdlog::error("[core::nats] Event-Loop setup failed: {}", natsStatus_GetText(status));
    m_isInitialized = false;
  }

  m_isInitialized = true;
}

EventLoop::~EventLoop() {
  disconnect();
  nats_Close();
}

boost::cobalt::promise<bool> EventLoop::connect(const core::client::Settings& settings) {
  std::ostringstream fullUrl;
  fullUrl << settings.serverUrl() << ":" << settings.port();
  std::string targetUrl = fullUrl.str();

  spdlog::info("[core::nats] Connecting to {}...", targetUrl);

  auto status = natsOptions_SetURL(m_opts, targetUrl.c_str());
  if (status != NATS_OK) {
    spdlog::error("[core::nats] SetURL failed: {}", natsStatus_GetText(status));
    co_return false;
  }

  // Simulate non-blocking handshake coroutine yield
  boost::asio::steady_timer handshakeTimer(m_executor, std::chrono::milliseconds(100));
  co_await handshakeTimer.async_wait(boost::cobalt::use_op);

  status = natsConnection_Connect(&conn_, m_opts);
  if (status != NATS_OK) {
    spdlog::warn("[core::nats] Could not connect to {}: {} (check if NATS server is running on {})", targetUrl,
                 natsStatus_GetText(status), targetUrl);
    isConnected_ = false;
    co_return false;
  }

  isConnected_ = true;
  spdlog::info("[core::nats] Connected to {}", targetUrl);
  co_return true;
}

boost::cobalt::promise<bool> EventLoop::publish(const std::string& subject, const std::string& payload) {
  if (!conn_ || !isConnected_) {
    spdlog::error("[core::nats] Cannot publish: not connected to NATS server.");
    co_return false;
  }

  natsStatus status = natsConnection_PublishString(conn_, subject.c_str(), payload.c_str());
  if (status != NATS_OK) {
    spdlog::error("[core::nats] Publish failed: {}", natsStatus_GetText(status));
    co_return false;
  }

  spdlog::info("[core::nats] Published message to [{}]: {}", subject, payload);
  co_return true;
}

void EventLoop::disconnect() {
  if (conn_) {
    natsConnection_Destroy(conn_);
    conn_ = nullptr;
  }
  if (m_opts) {
    natsOptions_Destroy(m_opts);
    m_opts = nullptr;
  }
  isConnected_ = false;
}

}  // namespace core::nats
