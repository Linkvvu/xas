#pragma once
#include "xas/Buffer.h"
#include "xas/ServerConfig.h"

#include <asio.hpp>
#include <atomic>
#include <deque>
#include <functional>
#include <string>
#include <system_error>

namespace xas {

class TcpSession : public std::enable_shared_from_this<TcpSession> {
public:
  explicit TcpSession(asio::ip::tcp::socket socket, const ServerConfig& config);

  // ── 公开 API ──────────────────────────────────────────────────────────
  uint64_t id() const { return id_; }
  std::string remoteAddress() const;
  uint16_t remotePort() const;
  bool isConnected() const { return connected_; }

  void send(const Buffer& data);
  void send(Buffer&& data);
  void close();

  // ── Framework-internal：由 TcpServer 在 start() 前注入 ───────────────
  void setRawCallback(std::function<void(SessionPtr, Buffer&)> cb);
  void
  setDisconnectCallback(std::function<void(SessionPtr, std::error_code)> cb);
  void setErrorCallback(std::function<void(SessionPtr, std::error_code)> cb);
  void setIdleCallback(std::function<void(SessionPtr)> cb);

  void start(); // 启动 async_read 循环和空闲定时器

private:
  void doRead();
  void doWrite();
  void resetIdleTimer();
  void onIdleTimeout(const std::error_code& ec);
  void forceClose(std::error_code reason);

  uint64_t id_;
  asio::ip::tcp::socket socket_;
  asio::strand<asio::any_io_executor> strand_;
  asio::steady_timer idleTimer_;
  ServerConfig config_;

  Buffer receiveBuffer_;
  std::deque<Buffer> writeQueue_;
  bool writing_ = false;
  bool closing_ = false;
  std::atomic<bool> connected_{true};

  std::function<void(SessionPtr, Buffer&)> rawCb_;
  std::function<void(SessionPtr, std::error_code)> disconnectCb_;
  std::function<void(SessionPtr, std::error_code)> errorCb_;
  std::function<void(SessionPtr)> idleCb_;

  static std::atomic<uint64_t> nextId_;
};

} // namespace xas
