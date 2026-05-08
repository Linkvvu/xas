# xas 实现任务报告

> 版本：0.1.0 | 生成日期：2026-05-08 | 基于：DESIGN.md

## 概述

本报告将 `xas`（C++17 异步 TCP 服务器框架）的实现拆解为 9 个独立任务，分 4 个阶段执行。同一阶段内的任务无依赖关系，可派发给多个 sub-agent 并行实现。

---

## 依赖关系总览

```
Phase 1 (并行)          Phase 2 (并行)     Phase 3 (串行)        Phase 4 (并行)
T1.1 ───────────┐
T1.2 ───────────┼──► T2.1 ──┬──► T3.1 ──► T3.2 ──┬──► T4.1
T1.3 ───────────┘    T2.2 ──┘                      └──► T4.2
```

| 任务 ID | 名称 | 阶段 | depends_on |
|---------|------|------|------------|
| T1.1 | 目录结构 + CMake | Phase 1 | — |
| T1.2 | Buffer.h | Phase 1 | — |
| T1.3 | ServerConfig.h | Phase 1 | — |
| T2.1 | TcpSession | Phase 2 | T1.1, T1.2, T1.3 |
| T2.2 | Pipeline + CodecHandle | Phase 2 | T1.1, T1.2 |
| T3.1 | TcpServer | Phase 3 | T2.1, T2.2 |
| T3.2 | xas.h (入口头文件) | Phase 3 | T3.1 |
| T4.1 | Echo 示例 | Phase 4 | T3.2 |
| T4.2 | TestEcho 测试 | Phase 4 | T3.2 |

---

## Phase 1 — 基础层（3 个任务可并行）

### T1.1 目录结构 + CMakeLists.txt 完善

**描述**：创建项目所有源码目录，补全根 `CMakeLists.txt` 使其包含所有子目录，并为 `src/`、`tests/`、`examples/echo/` 添加各自的 `CMakeLists.txt` 骨架。

**需创建/修改的文件**：
```
include/xas/           (目录，空)
src/                   (目录，空)
tests/CMakeLists.txt
examples/echo/         (目录，空)
CMakeLists.txt         (修改：追加 add_subdirectory)
```

**关键实现要点**：
- 根 `CMakeLists.txt` 需追加：
  ```cmake
  add_subdirectory(src)
  add_subdirectory(tests)
  add_subdirectory(examples/echo)
  ```
- `src/CMakeLists.txt` 定义静态库目标 `xas`，源文件为 `TcpServer.cpp` 和 `TcpSession.cpp`，include 路径指向 `${PROJECT_SOURCE_DIR}/include`。
- `tests/CMakeLists.txt` 链接 `xas` 库和 `GTest::gtest_main`。
- `examples/echo/CMakeLists.txt` 链接 `xas` 库。

**验收标准**：
- `cmake --preset msvc2017` 配置阶段无报错（源文件暂缺时允许链接报错，配置不报错即通过）。
- 目录结构与 `DESIGN.md` 中 Directory Structure 章节一致。

---

### T1.2 include/xas/Buffer.h

**描述**：定义 `xas` 命名空间下的基础类型别名。

**需创建的文件**：
```
include/xas/Buffer.h
```

**关键实现要点**：
```cpp
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace xas {

using Buffer      = std::vector<uint8_t>;
class TcpSession;
using SessionPtr  = std::shared_ptr<TcpSession>;
using WeakSession = std::weak_ptr<TcpSession>;

} // namespace xas
```
- 前向声明 `TcpSession` 以避免循环依赖。
- 不引入任何其他头文件。

**验收标准**：
- `#include "xas/Buffer.h"` 在空的 `.cpp` 中编译无警告。
- `xas::Buffer`、`xas::SessionPtr`、`xas::WeakSession` 类型可用。

---

### T1.3 include/xas/ServerConfig.h

**描述**：定义 `ServerConfig` 配置结构体，包含线程数、连接限制、超时、Socket 选项、日志器、Session 工厂。

**需创建的文件**：
```
include/xas/ServerConfig.h
```

**关键实现要点**：
```cpp
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include "xas/Buffer.h"   // for SessionPtr

// forward declaration for spdlog
namespace spdlog { class logger; }

// forward declaration for asio
namespace asio { namespace ip { class tcp; } }

namespace xas {

struct ServerConfig {
    uint32_t threadCount        = std::thread::hardware_concurrency();
    uint32_t maxConnections     = 10000;
    uint32_t maxReceiveBuffer   = 4 * 1024 * 1024;  // 4 MB
    uint32_t idleTimeoutSec     = 60;
    uint32_t shutdownTimeoutSec = 5;
    bool     tcpNoDelay         = true;
    bool     keepAlive          = true;

    std::shared_ptr<spdlog::logger> logger;

    using SessionFactory = std::function<SessionPtr(asio::ip::tcp::socket&&)>;
    SessionFactory sessionFactory;
};

} // namespace xas
```
- `SessionFactory` 使用前向声明，避免在此头文件中引入 ASIO 完整头文件。

**验收标准**：
- `ServerConfig` 默认构造可用，各字段默认值与 `DESIGN.md` 一致。
- 未提供 `logger` 和 `sessionFactory` 时值为 `nullptr`（由 `TcpServer` 实现中补充默认值）。

---

## Phase 2 — 核心类型层（2 个任务可并行，均依赖 Phase 1 完成）

### T2.1 include/xas/TcpSession.h + src/TcpSession.cpp

**描述**：实现每连接的会话类，包含异步读循环、Strand 序列化写队列、空闲定时器、接收缓冲限制检查。

**depends_on**：T1.1, T1.2, T1.3

**需创建的文件**：
```
include/xas/TcpSession.h
src/TcpSession.cpp
```

**关键实现要点**：

*头文件公开接口*：
```cpp
class TcpSession : public std::enable_shared_from_this<TcpSession> {
public:
    uint64_t    id()            const;
    std::string remoteAddress() const;
    uint16_t    remotePort()    const;
    bool        isConnected()   const;

    void send(const Buffer& data);
    void send(Buffer&& data);
    void close();

    // Framework-internal: set by TcpServer after construction
    void setRawCallback(std::function<void(SessionPtr, Buffer&)> cb);
    void setConfig(const ServerConfig& cfg);
    void start();  // begin async_read loop + idle timer
};
```

*实现要点*：
- **写队列**：`std::deque<Buffer> writeQueue_`，所有写操作通过 `asio::strand` post，`writing_` 标志防止并发 `async_write`。
- **异步读循环**：`async_read_some` → 追加数据到 `receiveBuffer_` → 检查 `maxReceiveBuffer` → 调用 `raw_cb_` → 重置空闲定时器 → 继续循环。
- **空闲定时器**：每次收到数据后重置；超时后调用 `onIdle` 回调（由 `TcpServer` 通过 `setIdleCallback` 注入）。
- **接收缓冲超限**：触发错误回调并强制 close。
- **`close()`**：先让写队列排空（`shutdown(send)`），再关闭 socket。
- **唯一 ID**：静态原子计数器 `std::atomic<uint64_t>`，构造时自增赋值。

**验收标准**：
- 可被 `TcpServer` 正确构造和管理。
- `send(Buffer&&)` 不发生额外拷贝（移动语义）。
- 空闲超时触发 `onIdle` 回调。
- 接收缓冲超限时 session 被关闭。

---

### T2.2 include/xas/Pipeline.h + include/xas/CodecHandle.h

**描述**：实现类型桥接层。`Pipeline<Codec>` 以 Codec 具体类型为模板参数，直接持有并调用 Codec 方法（duck typing，无虚函数）。`CodecHandle<T>` 是暴露给用户的公开 API，通过 `std::function` 持有编解码能力，与 Codec 具体类型解耦。

**depends_on**：T1.1, T1.2

**需创建的文件**：
```
include/xas/Pipeline.h
include/xas/CodecHandle.h
```

**关键实现要点**：

*Pipeline.h*：
```cpp
// Pipeline 以 Codec 为模板参数，直接调用其方法——无基类，无虚函数
template<typename Codec>
class Pipeline {
    using T = typename Codec::MessageType;
public:
    using TypedCb = std::function<void(SessionPtr, T)>;

    explicit Pipeline(std::shared_ptr<Codec> codec) : codec_(std::move(codec)) {}

    void setMessageCb(TypedCb cb) { cb_ = std::move(cb); }

    // Called by TcpSession's raw_cb_
    void process(SessionPtr sess, Buffer& buf) {
        while (auto msg = codec_->decode(buf))
            if (cb_) cb_(sess, std::move(*msg));
    }

    Buffer encode(const T& msg) { return codec_->encode(msg); }

private:
    std::shared_ptr<Codec> codec_;   // 直接持有具体类型，编译期鸭子类型检查
    TypedCb cb_;
};
```

*CodecHandle.h*：
```cpp
// CodecHandle<T> 不知道 Codec 具体类型；通过 std::function 持有能力，
// 在 TcpServer::setCodec() 实例化时从 Pipeline<Codec> 捕获。
template<typename T>
class CodecHandle {
public:
    using TypedCb = std::function<void(SessionPtr, T)>;

    CodecHandle(std::function<void(TypedCb)>    registerCb,
                std::function<Buffer(const T&)> encodeFn)
        : registerCb_(std::move(registerCb))
        , encode_(std::move(encodeFn)) {}

    void onMessage(TypedCb cb) {
        registerCb_(std::move(cb));
    }

    void sendMsg(SessionPtr sess, const T& msg) {
        sess->send(encode_(msg));
    }

private:
    std::function<void(TypedCb)>    registerCb_;
    std::function<Buffer(const T&)> encode_;
};
```

*TcpServer::setCodec\<Codec\>() 对应实现*（在 T3.1 中完成，此处列出以说明桥接方式）：
```cpp
template<typename Codec>
CodecHandle<typename Codec::MessageType> setCodec(std::shared_ptr<Codec> codec) {
    using T = typename Codec::MessageType;
    auto pipeline = std::make_shared<Pipeline<Codec>>(std::move(codec));
    // 类型擦除发生在此 lambda 处，而非通过虚函数
    rawCb_ = [pipeline](SessionPtr s, Buffer& b) { pipeline->process(s, b); };
    return CodecHandle<T>(
        [pipeline](std::function<void(SessionPtr, T)> cb) {
            pipeline->setMessageCb(std::move(cb));
        },
        [pipeline](const T& msg) { return pipeline->encode(msg); }
    );
}
```

- Codec 不需要继承任何基类；编译期若缺少 `decode`/`encode`/`MessageType` 则报错。
- 类型擦除边界为 `raw_cb_`（`std::function<void(SessionPtr, Buffer&)>`），不依赖虚函数。
- `TcpServer` 和 `TcpSession` 保持非模板。

**验收标准**：
- 满足 duck typing 契约的自定义 Codec 无需继承任何基类即可编译通过。
- 缺少 `decode`/`encode`/`MessageType` 之一的 Codec 在 `setCodec()` 处产生编译错误（而非运行时错误）。
- `sendMsg` 正确调用 `encode` 并通过 `SessionPtr::send` 发送。
- `TcpSession` 的 `raw_cb_` 可绑定为 `[pipeline](SessionPtr s, Buffer& b){ pipeline->process(s,b); }`。

---

## Phase 3 — 服务器层（串行，依赖 Phase 2 完成）

### T3.1 include/xas/TcpServer.h + src/TcpServer.cpp

**描述**：实现服务器主体，包含接受器循环、会话 Map、线程池管理、全部生命周期回调注入、两阶段优雅关闭、`setCodec<T>()` 模板方法。

**depends_on**：T2.1, T2.2

**需创建的文件**：
```
include/xas/TcpServer.h
src/TcpServer.cpp
```

**关键实现要点**：

*头文件公开接口*（与 DESIGN.md 一致）：
```cpp
class TcpServer {
public:
    TcpServer(std::string host, uint16_t port, ServerConfig config = {});
    ~TcpServer();

    void onConnect(std::function<void(SessionPtr)> cb);
    void onDisconnect(std::function<void(SessionPtr, std::error_code)> cb);
    void onError(std::function<void(SessionPtr, std::error_code)> cb);
    void onIdle(std::function<void(SessionPtr)> cb);
    void onOverload(std::function<void()> cb);

    template<typename Codec>
    CodecHandle<typename Codec::MessageType> setCodec(std::shared_ptr<Codec> codec);

    void start();
    void run();
    void stop();
    void wait();

    SessionPtr getSession(uint64_t id);
    size_t     sessionCount() const;
};
```

*实现要点*：
- **单 `io_context` + 线程池**：`std::vector<std::thread>`，每线程调用 `ioc_.run()`。
- **接受器循环**：`async_accept` → 检查 `maxConnections`（超限则关闭并触发 `onOverload`）→ 构造 `TcpSession`（或调用 `sessionFactory`）→ 注入所有回调 → `session->start()` → 继续接受。
- **会话 Map**：`std::unordered_map<uint64_t, SessionPtr>`，用 `asio::strand` 保护。
- **两阶段优雅关闭**（`stop()`）：
  1. 关闭接受器，停止接受新连接。
  2. 遍历所有 session 调用 `close()`，等待写队列排空。若超过 `shutdownTimeoutSec`，强制关闭剩余 session。
- **默认 logger**：若 `config_.logger == nullptr`，构造时创建 `spdlog::stdout_color_mt("xas")`。
- **`setCodec<T>()` 实现**：
  ```cpp
  template<typename Codec>
  CodecHandle<typename Codec::MessageType> setCodec(std::shared_ptr<Codec> codec) {
      using T = typename Codec::MessageType;
      auto pipeline = std::make_shared<Pipeline<Codec>>(std::move(codec));
      rawCb_ = [pipeline](SessionPtr s, Buffer& b) { pipeline->process(s, b); };
      return CodecHandle<T>(
          [pipeline](std::function<void(SessionPtr, T)> cb) {
              pipeline->setMessageCb(std::move(cb));
          },
          [pipeline](const T& msg) { return pipeline->encode(msg); }
      );
  }
  ```

**验收标准**：
- `server.run()` 可在指定端口监听并接受连接。
- `stop()` 后所有 session 关闭，线程池退出，`wait()` 返回。
- 连接数超 `maxConnections` 时触发 `onOverload`，不崩溃。
- `getSession(id)` 返回正确的 `SessionPtr` 或 `nullptr`。

---

### T3.2 include/xas/xas.h

**描述**：提供 all-in-one 入口头文件，`#include "xas/xas.h"` 即可使用全部公开 API。

**depends_on**：T3.1

**需创建的文件**：
```
include/xas/xas.h
```

**关键实现要点**：
```cpp
#pragma once
#include "xas/Buffer.h"
#include "xas/ServerConfig.h"
#include "xas/TcpSession.h"
#include "xas/Pipeline.h"
#include "xas/CodecHandle.h"
#include "xas/TcpServer.h"
```
- 头文件顺序遵循依赖关系（Buffer → Config → Session → Pipeline → CodecHandle → Server）。
- 不添加任何逻辑代码。

**验收标准**：
- 单独 `#include "xas/xas.h"` 在空 `.cpp` 中编译无报错、无警告。

---

## Phase 4 — 验证层（2 个任务可并行，均依赖 T3.2 完成）

### T4.1 examples/echo/main.cpp

**描述**：实现 Echo 示例服务器，演示框架完整用法：配置、回调注册、Codec 注入、消息回显。

**depends_on**：T3.2

**需创建的文件**：
```
examples/echo/main.cpp
examples/echo/CMakeLists.txt  (若 T1.1 未创建则在此补充)
```

**关键实现要点**：

实现与 `DESIGN.md` Usage Example 章节完全一致的 Echo 服务器。需自定义一个最简 `EchoCodec`（直接原样回传字节，`MessageType = xas::Buffer`）以避免依赖外部协议：

```cpp
struct EchoCodec {
    using MessageType = xas::Buffer;
    std::optional<xas::Buffer> decode(xas::Buffer& buf) {
        if (buf.empty()) return std::nullopt;
        xas::Buffer msg = std::move(buf);
        buf.clear();
        return msg;
    }
    xas::Buffer encode(const xas::Buffer& msg) { return msg; }
};
```

**验收标准**：
- 编译通过，启动后可用 `telnet localhost 8080` 连接并收到原样回显。
- 连接断开后打印 disconnect 日志。

---

### T4.2 tests/TestEcho.cpp + tests/CMakeLists.txt

**描述**：使用 GoogleTest 编写集成测试，通过本地 TCP 连接验证框架核心行为。

**depends_on**：T3.2

**需创建的文件**：
```
tests/TestEcho.cpp
tests/CMakeLists.txt  (若 T1.1 未创建则在此补充)
```

**测试用例列表**：

| 测试名 | 验证内容 |
|--------|----------|
| `EchoServer.ConnectAndEcho` | 连接后发送数据，收到原样回显 |
| `EchoServer.OnConnectCallback` | 连接时触发 `onConnect` 回调 |
| `EchoServer.OnDisconnectCallback` | 断开连接时触发 `onDisconnect` 回调，`error_code` 正确 |
| `EchoServer.IdleTimeout` | 空闲超过 `idleTimeoutSec` 后 session 被关闭 |
| `EchoServer.MaxConnectionsOverload` | 连接数达到上限时触发 `onOverload`，不崩溃 |
| `EchoServer.GracefulShutdown` | `stop()` 后服务器干净退出，活跃连接被关闭 |

**实现约定**：
- 每个测试用例启动独立的 `TcpServer`（随机端口），测试结束后 `stop()`。
- 使用阻塞 `connect` + 同步 `send/recv`（POSIX socket 或 ASIO sync API）作为客户端。
- 超时保护：每个测试 5 秒 deadline，防止死锁挂住 CI。

**验收标准**：
- `ctest` 全部 6 个测试用例通过。
- 测试不依赖外部服务，完全本地运行。

---

## 实施建议

1. **Phase 1 全并行**：T1.1 / T1.2 / T1.3 可同时派发给 3 个独立 sub-agent。
2. **Phase 2 全并行**：Phase 1 完成后，T2.1 / T2.2 同时派发。
3. **Phase 3 串行**：T3.1 完成后再执行 T3.2（T3.2 极小，可合并入 T3.1 agent）。
4. **Phase 4 全并行**：T3.2 完成后，T4.1 / T4.2 同时派发。
5. 每个任务的 agent 上下文中应包含：`DESIGN.md` 全文 + 本报告对应任务条目 + 已完成依赖任务的文件内容。
