# xas Design Document

> Version: 0.1.0 | C++17 | Standalone ASIO

## Goals

| 目标 | 说明 |
|------|------|
| 开箱即用 | 合理默认值，最少配置即可运行 |
| 功能完善 | 连接管理、帧解析、优雅关闭、空闲超时、错误分级 |
| 高扩展性 | 回调注入、Codec duck typing、Session 工厂钩子 |
| 高性能 | 单 io_context + 线程池 + strand，无锁写队列，零拷贝发送 |

---

## Architecture Overview

```
┌─────────────────────────────────────────────────┐
│                   User Code                     │
│  server.onConnect / onDisconnect / onIdle        │
│  pipeline.onMessage([](SessionPtr, MyMsg) {})    │
└────────────────┬────────────────────────────────┘
                 │ callbacks
┌────────────────▼────────────────────────────────┐
│                  TcpServer                      │
│  acceptor loop  │  session map  │  thread pool  │
└────────────────┬────────────────────────────────┘
                 │ owns
┌────────────────▼────────────────────────────────┐
│                  TcpSession                     │
│  async_read → raw_cb_  │  write queue + strand  │
└────────────────┬────────────────────────────────┘
                 │ raw_cb_ (type-erased)
┌────────────────▼────────────────────────────────┐
│              Pipeline<T>                        │
│  codec.decode(buf) → typed msg → onMessage cb   │
└────────────────┬────────────────────────────────┘
                 │ duck typing
┌────────────────▼────────────────────────────────┐
│           User Codec (self-impl)                │
│  decode(Buffer&) → optional<T>                  │
│  encode(const T&) → vector<uint8_t>             │
└─────────────────────────────────────────────────┘
```

---

## Directory Structure

```
xas/
├── include/
│   └── xas/
│       ├── xas.h            # all-in-one entry: #include all headers below
│       ├── TcpServer.h
│       ├── TcpSession.h
│       ├── Pipeline.h
│       ├── CodecHandle.h
│       ├── ServerConfig.h
│       └── Buffer.h
├── src/
│   ├── TcpServer.cpp
│   └── TcpSession.cpp
├── tests/
│   ├── CMakeLists.txt
│   └── TestEcho.cpp
├── examples/
│   └── echo/
│       └── main.cpp
├── CMakeLists.txt
├── vcpkg.json
└── DESIGN.md
```

---

## Core Types

```cpp
namespace xas {

using Buffer     = std::vector<uint8_t>;
using SessionPtr = std::shared_ptr<TcpSession>;
using WeakSession = std::weak_ptr<TcpSession>;

}
```

---

## ServerConfig

```cpp
namespace xas {

struct ServerConfig {
    // Threading
    uint32_t threadCount         = std::thread::hardware_concurrency();

    // Connection limits
    uint32_t maxConnections      = 10000;
    uint32_t maxReceiveBuffer    = 4 * 1024 * 1024;  // 4MB per session

    // Timeouts (seconds, 0 = disabled)
    uint32_t idleTimeoutSec      = 60;
    uint32_t shutdownTimeoutSec  = 5;

    // Socket options
    bool tcpNoDelay              = true;
    bool keepAlive               = true;

    // Logging (nullptr = create default "xas" logger)
    std::shared_ptr<spdlog::logger> logger;

    // Session factory (nullptr = default TcpSession; inject for TLS in v0.2)
    using SessionFactory = std::function<SessionPtr(asio::ip::tcp::socket&&)>;
    SessionFactory sessionFactory;
};

}
```

---

## TcpServer

```cpp
namespace xas {

class TcpServer {
public:
    TcpServer(std::string host, uint16_t port, ServerConfig config = {});
    ~TcpServer();

    // ── Lifecycle callbacks ──────────────────────────────────────────────
    void onConnect(std::function<void(SessionPtr)> cb);
    void onDisconnect(std::function<void(SessionPtr, std::error_code)> cb);

    // Codec parse error: user decides whether to close the session
    void onError(std::function<void(SessionPtr, std::error_code)> cb);

    // Called when a session has had no inbound data for idleTimeoutSec
    void onIdle(std::function<void(SessionPtr)> cb);

    // Called when a new connection is rejected due to maxConnections
    void onOverload(std::function<void()> cb);

    // ── Codec setup ──────────────────────────────────────────────────────
    // Returns a CodecHandle<T> for registering typed onMessage callback.
    // Codec must satisfy the duck-typing contract (see Codec Contract below).
    template<typename Codec>
    CodecHandle<typename Codec::MessageType> setCodec(std::shared_ptr<Codec> codec);

    // ── Server control ───────────────────────────────────────────────────
    void start();   // non-blocking: launches thread pool, begins accepting
    void run();     // blocking:     start() + wait()
    void stop();    // initiates two-phase graceful shutdown
    void wait();    // blocks until server has fully stopped

    // ── Session access ───────────────────────────────────────────────────
    SessionPtr getSession(uint64_t id);   // returns nullptr if not found
    size_t     sessionCount() const;
};

}
```

### Two-Phase Graceful Shutdown (`stop()`)

1. **Phase 1** — Stop acceptor; reject new connections.
2. **Phase 2** — Wait for all session write queues to drain. If any session exceeds `shutdownTimeoutSec`, force-close it.

---

## TcpSession

```cpp
namespace xas {

class TcpSession : public std::enable_shared_from_this<TcpSession> {
public:
    // ── Identity ─────────────────────────────────────────────────────────
    uint64_t    id()            const;
    std::string remoteAddress() const;
    uint16_t    remotePort()    const;
    bool        isConnected()   const;

    // ── Send ─────────────────────────────────────────────────────────────
    void send(const Buffer& data);   // copies data into write queue
    void send(Buffer&& data);        // moves data into write queue (zero-copy)

    // ── Control ──────────────────────────────────────────────────────────
    void close();   // initiates graceful half-close; drains write queue first
};

}
```

### Session Lifecycle

```
accept()
   │
   ▼
onConnect(sess)
   │
   ├─── async_read loop ───► raw_cb_(sess, buf) ───► Pipeline::process()
   │                                                       │
   │                                              codec.decode(buf)
   │                                                       │
   │                                           onMessage(sess, typedMsg)
   │
   ├─── idle timer ────────────────────────────► onIdle(sess)
   │
   └─── EOF / error / close() ─────────────────► onDisconnect(sess, ec)
```

### Write Queue + Strand

`send()` is thread-safe. All write operations are serialised via `asio::strand`:

```
send(data)
   │
   └─► strand.post([data] {
           writeQueue_.push_back(data);
           if (!writing_) doWrite();
       })

doWrite()
   └─► async_write(socket_, writeQueue_.front(), handler)
           └─► on complete: pop front → if !empty: doWrite()
```

### Receive Buffer Limit

After each `async_read` completion, the framework checks:

```cpp
if (receiveBuffer_.size() > config_.maxReceiveBuffer) {
    // trigger onError, then close session
}
```

---

## Codec Contract (Duck Typing)

No base class required. A valid Codec type must provide:

```cpp
class MyCodec {
public:
    using MessageType = MyMessage;

    // Framing + deserialization.
    // Consume bytes from buf and return a message if a complete frame is ready.
    // Return std::nullopt if more data is needed (do NOT consume partial frame).
    std::optional<MessageType> decode(xas::Buffer& buf);

    // Serialization + framing.
    // Return the fully framed bytes ready to send over the wire.
    xas::Buffer encode(const MessageType& msg);
};
```

The framework never touches `MessageType` directly — it lives only between `Pipeline<T>` and the user's `onMessage` callback.

---

## Pipeline\<T\> (Internal)

Bridges the type-erased TcpSession layer and the user's typed callback. Not part of the public API — users interact with it through `CodecHandle<T>`.

```cpp
template<typename T>
class Pipeline {
public:
    using TypedCb = std::function<void(SessionPtr, T)>;

    void setMessageCb(TypedCb cb) { cb_ = std::move(cb); }

    // Called by TcpSession's raw_cb_ on each read
    void process(SessionPtr sess, Buffer& buf) {
        while (auto msg = codec_->decode(buf)) {
            if (cb_) cb_(sess, std::move(*msg));
        }
    }

    Buffer encode(const T& msg) { return codec_->encode(msg); }

private:
    std::shared_ptr<CodecImpl> codec_;
    TypedCb cb_;
};
```

**Type erasure path:**

```
TcpSession::raw_cb_
  = std::function<void(SessionPtr, Buffer&)>
  = [pipeline](SessionPtr s, Buffer& b) { pipeline->process(s, b); }
```

TcpServer and TcpSession remain non-templated.

---

## CodecHandle\<T\>

Returned by `TcpServer::setCodec()`. Provides the typed API surface for the user.

```cpp
template<typename T>
class CodecHandle {
public:
    // Register typed message callback
    void onMessage(std::function<void(SessionPtr, T)> cb);

    // Encode msg and send via sess (convenience wrapper)
    void sendMsg(SessionPtr sess, const T& msg);
};
```

---

## Callback Reference

| Callback | 注册位置 | 触发时机 |
|----------|----------|----------|
| `onConnect` | TcpServer | 新连接建立，session 已就绪 |
| `onDisconnect` | TcpServer | 连接断开（主动/被动/错误），携带 `error_code` |
| `onError` | TcpServer | Codec decode 失败；用户决定是否 close |
| `onIdle` | TcpServer | session 超过 `idleTimeoutSec` 无入站数据 |
| `onOverload` | TcpServer | 连接数达到 `maxConnections`，新连接被拒绝 |
| `onMessage` | CodecHandle | 完整帧 decode 成功，强类型消息就绪 |

---

## Error Handling

| 错误类型 | 行为 |
|----------|------|
| 网络错误 (EOF, ECONNRESET, ...) | 框架强制关闭 session，触发 `onDisconnect(sess, ec)` |
| Codec decode 失败 | 触发 `onError(sess, ec)`，用户决定是否 `sess->close()` |
| 接收缓冲超限 | 触发 `onError`，框架强制关闭 session |
| 连接数超限 | accept 后立即 close，触发 `onOverload()` |

---

## Namespace & Headers

```cpp
// All-in-one
#include "xas/xas.h"

// Or per-component
#include "xas/TcpServer.h"
#include "xas/TcpSession.h"
#include "xas/Pipeline.h"
#include "xas/ServerConfig.h"
#include "xas/Buffer.h"
```

All symbols live in the `xas::` namespace.

---

## Usage Example

```cpp
#include "xas/xas.h"
#include "MyCodec.h"   // user-implemented, satisfies Codec contract

int main() {
    xas::ServerConfig config;
    config.threadCount    = 4;
    config.idleTimeoutSec = 30;

    xas::TcpServer server("0.0.0.0", 8080, config);

    server.onConnect([](xas::SessionPtr sess) {
        spdlog::info("connected: {} id={}", sess->remoteAddress(), sess->id());
    });

    server.onDisconnect([](xas::SessionPtr sess, std::error_code ec) {
        spdlog::info("disconnected: id={} reason={}", sess->id(), ec.message());
    });

    server.onIdle([](xas::SessionPtr sess) {
        sess->close();  // or send heartbeat
    });

    auto pipeline = server.setCodec(std::make_shared<MyCodec>());

    pipeline.onMessage([&pipeline](xas::SessionPtr sess, MyMessage msg) {
        // echo back
        pipeline.sendMsg(sess, msg);
    });

    server.run();  // blocking
}
```

---

## v0.2 Roadmap

| 特性 | 扩展点 |
|------|--------|
| TLS/SSL | `ServerConfig::sessionFactory` 注入 `ssl::stream<tcp::socket>` 包装的 session |
| 多监听端口 | 多个 `TcpServer` 实例共享同一 `io_context`（预留 `io_context` 注入接口） |
| 客户端连接器 | 独立 `TcpClient` 类，复用 Codec / Pipeline 层 |
