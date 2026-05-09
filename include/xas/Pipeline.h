#pragma once
#include "xas/Buffer.h"
#include "xas/MessageTrait.h"
#include "xas/error.h"

#include <functional>
#include <map>
#include <memory>
#include <tl/expected.hpp>

namespace xas {

// Pipeline<Codec> 以 Codec 的具体类型为模板参数。
// 直接调用 codec_->decode() / codec_->encode()，无虚函数。
template <typename Codec>
class Pipeline {
public:
  using T       = typename Codec::MessageType;
  using TypedCb = std::function<void(SessionPtr, T)>;

  explicit Pipeline(std::shared_ptr<Codec> codec)
      : codec_(std::move(codec))
  {
  }

  void setMessageCb(TypedCb cb) { cb_ = std::move(cb); }

  void setErrorCb(std::function<void(SessionPtr, std::error_code)> cb) {
    errorCb_ = std::move(cb);
  }

  // 新增 route 方法
  void route(uint16_t cmd, TypedCb cb) { routes_[cmd] = std::move(cb); }
  void routeDefault(TypedCb cb) { defaultCb_ = std::move(cb); }

  // 由 TcpSession 的 raw_cb_ 调用
  void process(SessionPtr sess, Buffer& buf)
  {
    while (true) {
      auto result = codec_->decode(buf);
      if (!result) {
        const auto& ec = result.error();
        if (ec == make_error_code(xas_errc::incomplete_data)) {
          return; // 数据不完整，等更多数据
        }
        // 无效格式 → 先触发 onError，再强制关闭 session
        if (errorCb_) errorCb_(sess, ec);
        sess->forceClose(ec);
        return;
      }
      if (cb_) {
        uint16_t key = MessageCmd<T>::extract(*result);
        auto it = routes_.find(key);
        if (it != routes_.end()) {
          it->second(sess, std::move(*result));
        } else if (defaultCb_) {
          defaultCb_(sess, std::move(*result));
        } else {
          cb_(sess, std::move(*result));
        }
      }
    }
  }

  Buffer encode(const T& msg) { return codec_->encode(msg); }

private:
  std::shared_ptr<Codec> codec_;
  TypedCb cb_;
  std::function<void(SessionPtr, std::error_code)> errorCb_;
  std::map<uint16_t, TypedCb> routes_;
  TypedCb defaultCb_;
};

} // namespace xas
