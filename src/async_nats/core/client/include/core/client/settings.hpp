#ifndef CORE_CLIENT_SETTINGS_HPP
#define CORE_CLIENT_SETTINGS_HPP

#include <cstddef>
#include <string>

namespace core::client {

class Settings {
 public:
  Settings() = default;
  Settings(std::string serverUrl, int port) : serverUrl_(std::move(serverUrl)), port_(port) {}

  const std::string& serverUrl() const { return serverUrl_; }
  void setServerUrl(std::string serverUrl) { serverUrl_ = std::move(serverUrl); }

  int port() const { return port_; }
  void setPort(int port) { port_ = port; }

  int ioThreads() const { return m_ioThreads; }
  void setIoThreads(size_t count) { m_ioThreads = count; }

  void setWorkerThreads(size_t count) { m_workderThreads = count; }
  int workerThreads() const { return m_workderThreads; }

 private:
  std::string serverUrl_{"nats://127.0.0.1"};
  int port_{4222};
  size_t m_ioThreads{1};
  size_t m_workderThreads{1};
};

}  // namespace core::client

#endif  // CORE_CLIENT_SETTINGS_HPP
