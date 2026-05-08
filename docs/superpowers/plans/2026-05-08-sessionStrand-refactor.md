# sessionStrand Removal & stop() Refactor — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 删除 `TcpServer::sessionStrand_` 成员并重构 `stop()` 流程，消除 work guard 和 strand 的 ordering 依赖。

**Architecture:** 移除 `sessionStrand_` 后，所有跨-session 操作统一通过 `sessionsMutex_` 保护。`stop()` 改为三阶段：acceptor close → asio::post 快照 + close sessions + 条件 arm shutdown timer → work guard reset。所有 session.close() 在 work guard reset 之前通过 asio::post 排入 ioc_ 队列，保证 ioc_.run() 不会提前返回。

**Tech Stack:** C++17, Boost ASIO (standalone asio.hpp), spdlog

---

## File Map

| 文件 | 改动 |
|------|------|
| `include/xas/TcpServer.h` | 删除 `sessionStrand_` 成员变量（L70） |
| `src/TcpServer.cpp` | 删除 `sessionStrand_` 初始化；重构 `stop()`；删除 `doAccept()` 中所有 `asio::post(sessionStrand_, ...)` |

无新文件，无测试文件改动。

---

## Tasks

### Task 1: 删除 TcpServer.h 中的 sessionStrand_ 成员

**Files:**
- Modify: `include/xas/TcpServer.h`

- [ ] **Step 1: 删除成员变量声明**

文件 `include/xas/TcpServer.h` 第 70 行，删除：

```cpp
asio::strand<asio::io_context::executor_type> sessionStrand_;
```

其他内容（callback 类型定义、其他成员）均不变。

- [ ] **Step 2: Commit**

```bash
git add include/xas/TcpServer.h
git commit -m "refactor: remove sessionStrand_ member from TcpServer.h"
```

---

### Task 2: 重构 TcpServer.cpp 构造函数 — 删除 sessionStrand_ 初始化

**Files:**
- Modify: `src/TcpServer.cpp`

- [ ] **Step 1: 删除初始化列表中的 sessionStrand_**

文件 `src/TcpServer.cpp` 构造函数初始化列表（第 15 行），删除：

```cpp
, sessionStrand_(asio::make_strand(ioc_))
```

- [ ] **Step 2: Commit**

```bash
git add src/TcpServer.cpp
git commit -m "refactor: remove sessionStrand_ initialization from TcpServer ctor"
```

---

### Task 3: 重构 doAccept() — 移除所有 asio::post(sessionStrand_, ...)

**Files:**
- Modify: `src/TcpServer.cpp`

当前 `doAccept()` 在两个地方使用 `sessionStrand_`：
1. 第 194 行：`asio::post(sessionStrand_, [this, sock = std::move(socket)]() mutable {` — session 创建逻辑
2. 第 220 行：`asio::post(sessionStrand_, [this, id = s->id()] { removeSession(id); });` — removeSession 调用

- [ ] **Step 1: 将 doAccept() lambda 中的 asio::post(sessionStrand_, ...) 改为 asio::post(ioc_, ...)**

第 194 行改为：

```cpp
asio::post(ioc_, [this, sock = std::move(socket)]() mutable {
```

第 220 行改为：

```cpp
asio::post(ioc_, [this, id = s->id()] { removeSession(id); });
```

注意：`ioc_` 是成员变量，在 lambda capture 中可直接用，lambda 无需额外捕获。

- [ ] **Step 2: Commit**

```bash
git add src/TcpServer.cpp
git commit -m "refactor: replace asio::post(sessionStrand_, ...) with asio::post(ioc_, ...) in doAccept()"
```

---

### Task 4: 重构 stop() — 三阶段流程

**Files:**
- Modify: `src/TcpServer.cpp`

当前 `stop()` 实现（`TcpServer.cpp:121-178`）的核心问题是 `workGuard_.reset()` 在 strand 回调执行前就调用了。新实现：

- [ ] **Step 1: 替换 stop() 实现**

删除整个现有的 `stop()` 实现（`TcpServer.cpp:121-178`），替换为：

```cpp
void TcpServer::stop()
{
  std::call_once(stopOnce_, [this] {
    // Phase 1: stop accepting new connections.
    std::error_code ec;
    acceptor_.close(ec);

    // Phase 2: snapshot sessions and initiate graceful close on each.
    // All close() calls are posted to ioc_ *before* the work guard is reset,
    // so ioc_.run() will not exit prematurely.
    asio::post(ioc_, [this] {
      std::vector<SessionPtr> snapshot;
      {
        std::lock_guard<std::mutex> lk(sessionsMutex_);
        snapshot.reserve(sessions_.size());
        for (auto& [id, sess] : sessions_) {
          snapshot.push_back(sess);
        }
      }

      for (auto& sess : snapshot) {
        sess->close();
      }

      // If all sessions already drained (e.g. clients disconnected before
      // stop() was called), skip the timer entirely.
      {
        std::lock_guard<std::mutex> lk(sessionsMutex_);
        if (sessions_.empty()) {
          // Phase 3: release work guard so ioc_.run() can exit.
          workGuard_.reset();
          return;
        }
      }

      // Arm the shutdown timer; cancelled early if all sessions drain first.
      auto timer = std::make_shared<asio::steady_timer>(ioc_);
      timer->expires_after(
          std::chrono::seconds(config_.shutdownTimeoutSec));
      timer->async_wait([this, timer](const std::error_code& ec) {
        if (ec)
          return;
        std::lock_guard<std::mutex> lk(sessionsMutex_);
        for (auto& [id, sess] : sessions_) {
          if (sess->isConnected()) {
            sess->close();
          }
        }
        // Phase 3: always release work guard after timer fires.
        workGuard_.reset();
      });
    });
  });
}
```

关键改动说明：
- `shutdownTimer_` 从类成员改为局部 `auto timer = std::make_shared<...>`，生命周期内嵌在 handler 中
- `workGuard_.reset()` 移至 strand 回调内部（而非回调执行前），保证 ioc_ 不会提前退出
- timer callback 通过 mutex 保护访问 sessions_，与 `removeSession()` 并发安全
- `removeSession()` 中的 `shutdownTimer_->cancel()` 删除（因为 timer 现在是局部 shared_ptr，无须 cancel）

- [ ] **Step 2: Commit**

```bash
git add src/TcpServer.cpp
git commit -m "refactor: rewrite stop() as three-phase, remove sessionStrand_ dependency"
```

---

### Task 5: 删除 TcpServer.cpp 中已无用的 shutdownTimer_ 成员（可选）

**Files:**
- Modify: `include/xas/TcpServer.h`
- Modify: `src/TcpServer.cpp`

- [ ] **Step 1: 删除 shutdownTimer_ 成员声明**

文件 `include/xas/TcpServer.h` 第 81 行，删除：

```cpp
std::shared_ptr<asio::steady_timer> shutdownTimer_;
```

- [ ] **Step 2: 验证 build 通过**

```bash
# Build from project root
cmake --build build --parallel 2>&1
```

预期：编译成功，无链接错误（shutdownTimer_ 已在 stop() 中不再使用）。

- [ ] **Step 3: Commit**

```bash
git add include/xas/TcpServer.h
git commit -m "refactor: remove shutdownTimer_ member (now local to stop())"
```

---

## Self-Review Checklist

1. **Spec coverage:** 每个 spec 需求都有对应 task：
   - 删除 sessionStrand_ 成员 → Task 1 + 2 + 3
   - stop() 三阶段流程 → Task 4
   - shutdownTimer_ 局部化 → Task 4 + 5
2. **Placeholder scan:** 无 TBD/TODO，内容完整
3. **类型一致性：** `TcpServer::sessionStrand_` 引用在 Task 1-3 中已全部清除；`shutdownTimer_` 成员在 Task 5 中删除；`removeSession()` 中无 timer 引用需调整（已确认无引用）

## Verification

完成所有 tasks 后，运行：

```bash
cmake --build build --parallel 2>&1 | head -50
ctest --test-dir build --output-on-failure 2>&1
```

预期：编译通过，所有测试通过。
