#pragma once
#include <functional>
#include <memory>
#include <optional>
#include "xas/Buffer.h"

namespace xas {

// Pipeline<Codec> 以 Codec 的具体类型为模板参数。
// 直接调用 codec_->decode() / codec_->encode()，无虚函数。
template<typename Codec>
class Pipeline {
public:
    using T       = typename Codec::MessageType;
    using TypedCb = std::function<void(SessionPtr, T)>;

    explicit Pipeline(std::shared_ptr<Codec> codec)
        : codec_(std::move(codec)) {}

    void setMessageCb(TypedCb cb) { cb_ = std::move(cb); }

    // 由 TcpSession 的 raw_cb_ 调用
    void process(SessionPtr sess, Buffer& buf) {
        while (auto msg = codec_->decode(buf)) {
            if (cb_) cb_(sess, std::move(*msg));
        }
    }

    Buffer encode(const T& msg) { return codec_->encode(msg); }

private:
    std::shared_ptr<Codec> codec_;
    TypedCb                cb_;
};

} // namespace xas
