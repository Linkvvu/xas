#pragma once
#include "xas/Buffer.h"
#include "xas/CodecHandle.h"
#include "xas/Pipeline.h"
#include "xas/ServerConfig.h"
#include "xas/TcpSession.h"

#include <asio.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

namespace xas {

using SessionFactory = std::function<SessionPtr(asio::ip::tcp::socket&&)>;

class TcpServer {
public:
  TcpServer(std::string host, uint16_t port, ServerConfig config = {});
  ~TcpServer();

  // ── 生命周期回调 ──────────────────────────────────────────────────────
  void onConnect(std::function<void(SessionPtr)> cb);
  void onDisconnect(std::function<void(SessionPtr, std::error_code)> cb);
  void onError(std::function<void(SessionPtr, std::error_code)> cb);
  void onIdle(std::function<void(SessionPtr)> cb);
  void onOverload(std::function<void()> cb);

  // ── Codec 注入 ────────────────────────────────────────────────────────
  template <typename Codec>
  CodecHandle<typename Codec::MessageType>
  setCodec(std::shared_ptr<Codec> codec)
  {
    using T       = typename Codec::MessageType;
    auto pipeline = std::make_shared<Pipeline<Codec>>(std::move(codec));
    rawCb_ = [pipeline](SessionPtr s, Buffer& b) { pipeline->process(s, b); };
    return CodecHandle<T>(
        [pipeline](std::function<void(SessionPtr, T)> cb) {
          pipeline->setMessageCb(std::move(cb));
        },
        [pipeline](const T& msg) { return pipeline->encode(msg); });
  }

  // ── 服务器控制 ────────────────────────────────────────────────────────
  bool start(); // 非阻塞：启动线程池，开始接受连接；重复调用返回 false
  bool run();   // 阻塞：start() + wait()；重复调用返回 false
  void stop();  // 发起两阶段优雅关闭
  void wait();  // 阻塞直到完全停止

  // ── 会话访问 ──────────────────────────────────────────────────────────
  SessionPtr getSession(uint64_t id);
  size_t sessionCount() const;

private:
  void doAccept();
  void addSession(SessionPtr sess);
  void removeSession(uint64_t id);

  std::string host_;
  uint16_t port_;
  ServerConfig config_;

  asio::io_context ioc_;
  asio::ip::tcp::acceptor acceptor_;
  asio::strand<asio::io_context::executor_type> sessionStrand_;

  mutable std::mutex sessionsMutex_;
  std::unordered_map<uint64_t, SessionPtr> sessions_;
  std::vector<std::thread> threads_;
  std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>>
      workGuard_;

  std::atomic<bool> started_{false};
  std::once_flag stopOnce_;
  std::once_flag waitOnce_;
  std::shared_ptr<asio::steady_timer> shutdownTimer_;

  std::function<void(SessionPtr, Buffer&)> rawCb_;
  std::function<void(SessionPtr)> connectCb_;
  std::function<void(SessionPtr, std::error_code)> disconnectCb_;
  std::function<void(SessionPtr, std::error_code)> errorCb_;
  std::function<void(SessionPtr)> idleCb_;
  std::function<void()> overloadCb_;
};

} // namespace xas
