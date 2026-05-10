#pragma once
#include "xas/Buffer.h"

#include <functional>

namespace xas {

// CodecHandle<T> 不知道 Codec 的具体类型。
// 通过两个 std::function 持有 Pipeline<Codec> 的能力，
// 在 TcpServer::setCodec<Codec>() 实例化时从 Pipeline 捕获。
template <typename T>
class CodecHandle {
public:
  using TypedCb = std::function<void(SessionPtr, T)>;

  CodecHandle(std::function<Buffer(const T&)> encodeFn,
              std::function<void(uint16_t, TypedCb)> routeCb,
              std::function<void(TypedCb)> routeDefaultCb)
      : encode_(std::move(encodeFn))
      , routeCb_(std::move(routeCb))
      , routeDefaultCb_(std::move(routeDefaultCb))
  {
  }

  // 注册路由
  void route(uint16_t cmd, TypedCb cb) {
    if (routeCb_) routeCb_(cmd, std::move(cb));
  }

  // 注册默认路由
  void routeDefault(TypedCb cb) {
    if (routeDefaultCb_) routeDefaultCb_(std::move(cb));
  }

  // 编码并通过 sess 发送
  void sendMsg(SessionPtr sess, const T& msg) { sess->send(encode_(msg)); }

private:
  std::function<Buffer(const T&)> encode_;
  std::function<void(uint16_t, TypedCb)> routeCb_;
  std::function<void(TypedCb)> routeDefaultCb_;
};

} // namespace xas
