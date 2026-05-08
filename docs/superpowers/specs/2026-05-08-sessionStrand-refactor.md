# Spec: 移除 TcpServer::sessionStrand_ 并重构 stop() 流程

> Date: 2026-05-08

## 背景

`TcpServer` 持有一个 `sessionStrand_`（`asio::strand<asio::io_context::executor_type>`），用于序列化所有跨-session 的操作（stop 时 close sessions、shutdown timer 等）。

### sessionStrand_ 的冗余性

1. `sessions_` 的读写已被 `sessionsMutex_` 保护，无需 strand 额外串行化
2. 每个 `TcpSession` 内部有自己的 `strand_`，其所有 I/O 已在自身 strand 上序列化
3. `sessionStrand_` 额外引入的串行保证对正确性没有增量贡献

### 当前 stop() 的问题

```cpp
void TcpServer::stop() {
  std::call_once(stopOnce_, [this] {
    acceptor_.close();
    workGuard_.reset();                        // ← work guard 先 reset

    asio::post(sessionStrand_, [this] {        // ← 但 strand 上的 handler 还未执行
      // close sessions + arm shutdownTimer_
    });
  });
}
```

`workGuard_.reset()` 在 strand 回调执行**之前**就被调用了。如果 strand 队列积压，`ioc_.run()` 可能提前返回，导致 `shutdownTimer_` 的 `async_wait` 不会触发。

## 目标

1. 删除 `sessionStrand_` 成员
2. 重构 `stop()` 流程，消除 work guard 和 strand 的 ordering 依赖
3. 保持所有正确性保证：sessions 仍被 mutex 保护，shutdown timeout 仍生效

---

## 设计

### 线程安全模型

移除 strand 后，所有跨-session 操作统一通过 `sessionsMutex_` 保护：

| 操作 | 保护方式 |
|------|----------|
| 遍历 sessions 快照 | `std::lock_guard<std::mutex>` |
| 调用 `sess->close()` | 各自 session 内部自有 strand |
| shutdown timer 回调 | `std::lock_guard<std::mutex>` |
| `removeSession()` | `std::lock_guard<std::mutex>` |

### stop() 新流程

```
stop() called
  │
  ▼
Phase 1: acceptor_.close()        ← 阻止新 accept（已有 mutex 无需保护）
  │
  ▼
Phase 2: asio::post(ioc_, ...)    ← 在 ioc_ 上排队一个 handler
         ├── 快照 session map（持 mutex）
         ├── 对每个 session 调用 close()
         │     close() 在各自 strand 上 drain write queue
         ├── 检查 sessions_ 是否已空
         │     ├── 为空 → 直接 reset workGuard_
         │     └── 非空 → arm shutdownTimer_
         │           timer 超时后：持 mutex 遍历 sessions_
         │                      对仍连接的 session 调用 forceClose()
         │                      reset workGuard_
         │
         ▼
Phase 3: workGuard_.reset()       ← session.close() 已全部 post 成功
```

**关键保证**：所有 `sess->close()` 在 `workGuard_.reset()` 之前通过 `asio::post()` 排入 ioc_ 队列。`workGuard_.reset()` 之后，ioc_ 仍有 work（这些 post 的 handler），所以 `ioc_.run()` 不会提前返回——等所有 handler 执行完毕才自然退出。

### shutdownTimer_ 的生命周期

- 在 `doAccept()` 的 lambda 中被创建并持有（而非类成员）
- 使用 `std::weak_ptr` 避免循环引用：timer 持有一个 weak 回调，session 的 disconnect 回调通过 `asio::post(ioc_, ...)` 原子性地在 timer callback 之前/之后正确处理

### 改动范围

| 文件 | 改动 |
|------|------|
| `include/xas/TcpServer.h` | 删除 `sessionStrand_` 成员变量 |
| `src/TcpServer.cpp` | 删除 `sessionStrand_` 构造初始化；重构 `stop()` 为上述三阶段流程；删除所有 `asio::post(sessionStrand_, ...)` 调用 |

`TcpSession.h/cpp` 无需改动。

---

## 行为对比

| 行为 | 旧 | 新 |
|------|----|----|
| work guard reset 时机 | 在 strand 回调执行前 | 在 strand 回调执行后 |
| 跨-session 并发 | 被 sessionStrand_ 串行化 | 允许并发（无额外必要串行化） |
| shutdown timer | 类成员，生命周期不明确 | 在 post handler 中创建，局部 |
| 正确性 | 依赖于 strand ordering | 等价保证，依赖 mutex + session internal strand |
