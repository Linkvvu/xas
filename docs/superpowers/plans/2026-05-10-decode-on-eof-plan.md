# TcpSession doRead 错误处理改进 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修改 `TcpSession::doRead()` 中 `ec` 分支逻辑：EOF 时先 decode 缓冲区再关闭，operation_aborted 时直接 return，其他错误立即关闭。

**Architecture:** 仅修改 `src/TcpSession.cpp` 中的 `doRead()` lambda 内 `ec` 分支，不新增文件，不改其他模块。

**Tech Stack:** C++17, Boost.Asio, GoogleTest

---

## Task 1: 修改 doRead() 错误处理逻辑

**Files:**
- Modify: `src/TcpSession.cpp:139-162`

- [ ] **Step 1: 确认当前代码**

当前 `doRead()` 中 `ec` 分支（L139-143）：
```cpp
if (ec) {
  self->forceClose(ec);
  return;
}
```

- [ ] **Step 2: 写入新实现**

替换为三级错误处理：
```cpp
if (ec) {
  if (ec == asio::error::eof) {
    // EOF：缓冲区数据完整，先 flush 再关闭
    if (self->rawCb_) {
      self->rawCb_(self->shared_from_this(), self->receiveBuffer_);
    }
  } else if (ec != asio::error::operation_aborted) {
    // operation_aborted：直接 return，不重复关闭
    // 其他错误：立即关闭
    self->forceClose(ec);
  }
  return;
}
```

- [ ] **Step 3: 编译验证**

Run: `cmake --build build --parallel`
Expected: 编译通过，无 error/warning

- [ ] **Step 4: 运行现有测试**

Run: `ctest --test-dir build --output-on-failure`
Expected: 所有现有测试通过（改动不破坏已有行为）

- [ ] **Step 5: 提交**

```bash
git add src/TcpSession.cpp
git commit -m "fix: flush receive buffer on EOF before closing session"
```

---

## Task 2: 新增测试用例 — EOF 时缓冲区数据被 decode

**Files:**
- Modify: `tests/TestEcho.cpp`

- [ ] **Step 1: 添加 Echo codec 的 raw callback 跟踪**

在 `EchoServer` fixture 中添加成员来记录 `rawCb_` 调用次数和接收到的最后一个 buffer。

在 `EchoCodec` 结构体外定义一个测试专用的 wrapper codec，包装 `EchoCodec` 并同时记录 `onRaw` 调用。

```cpp
// 在 TestEcho.cpp 中添加（在 SyncClient 定义之后、EchoServer fixture 定义之前）

static std::vector<xas::Buffer> g_rawReceived;
static void clearRawReceived() { g_rawReceived.clear(); }
```

- [ ] **Step 2: 编写新测试用例 FlushOnEof**

在 `GracefulShutdown` 测试之后新增：

```cpp
// ─────────────────────────────────────────────────────────────────────────────
// 7. FlushOnEof
//    Client sends data then close() without recv; server must decode the
//    trailing data before closing the session.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(EchoServer, FlushOnEof)
{
  const uint16_t port = getAvailablePort();

  auto handle = makeEchoServer(port);

  // Capture raw buffer flushes via a custom codec wrapper.
  struct CapturingCodec : EchoCodec {
    std::optional<xas::Buffer> decode(xas::Buffer& buf) override
    {
      if (!buf.empty()) {
        g_rawReceived.push_back(buf);
      }
      return EchoCodec::decode(buf);
    }
  };

  auto codec = std::make_shared<CapturingCodec>();
  server_->setCodec(codec);

  std::promise<xas::Buffer> msgPromise;
  auto msgFuture = msgPromise.get_future();

  handle.onMessage([&](xas::SessionPtr sess, xas::Buffer msg) {
    // Echo the message so the server actually reads it.
    handle.sendMsg(sess, msg);
    msgPromise.set_value(msg);
  });

  server_->start();

  SyncClient client;
  ASSERT_NO_THROW(client.connect(port));

  const std::vector<uint8_t> payload = {1, 2, 3, 4, 5};
  ASSERT_NO_THROW(client.write(payload));

  // Close the connection immediately after writing — no recv.
  // Server receives EOF with buffered data in receiveBuffer_.
  client.close();

  // Wait for the message to be processed.
  ASSERT_TRUE(waitFor(msgFuture));
  auto msg = msgFuture.get();

  // Verify the raw callback received the buffer.
  EXPECT_EQ(g_rawReceived.size(), 1u);
  EXPECT_EQ(g_rawReceived[0], payload);
  EXPECT_EQ(msg, payload);

  // Clean up captured state for other tests.
  g_rawReceived.clear();
}
```

- [ ] **Step 3: 编译测试**

Run: `cmake --build build --parallel`
Expected: 编译通过

- [ ] **Step 4: 运行新测试**

Run: `ctest --test-dir build -R FlushOnEof --output-on-failure`
Expected: PASS

- [ ] **Step 5: 运行全部测试**

Run: `ctest --test-dir build --output-on-failure`
Expected: 全部 PASS

- [ ] **Step 6: 提交**

```bash
git add tests/TestEcho.cpp
git commit -m "test: add FlushOnEof test for buffer flush on connection close"
```

---

## Self-Review

- **Spec coverage:** 三级错误处理逻辑已完整实现，EOF decode + close、operation_aborted return、其他错误 immediate forceClose，无遗漏。
- **Placeholder scan:** 无 TBD/TODO，代码即具体实现。
- **Type consistency:** `rawCb_` 调用签名与现有代码一致（`self->rawCb_(self->shared_from_this(), self->receiveBuffer_)`），与 L156 现有调用完全一致。