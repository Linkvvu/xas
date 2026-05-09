# TcpSession doRead 错误处理改进 — EOF 时先 Decode 再关闭

## 背景

`TcpSession::doRead()` 在 `asio::async_read` 完成时，若发生错误（`ec` 为真），会立即调用 `forceClose` 关闭连接，**不处理接收缓冲区中已有的数据**。

这个行为过于粗暴。常见场景（如 peer 正常关闭连接，EOF）下，缓冲区中的数据是完整且有效的，应该被 decode 后再关闭，而不是丢弃。

## 设计

### 修改范围

仅 `src/TcpSession.cpp` 的 `doRead()` 中 `ec` 分支。

### 三级错误处理逻辑

| 错误类型 | 处理方式 |
|----------|----------|
| `asio::error::eof` | **先调用 `rawCb_` flush 缓冲区，再 `forceClose`** |
| `asio::error::operation_aborted` | **直接 return**（stop() 主动取消，session 已在关闭流程中，无需重复关闭） |
| 其他错误 | 立即 `forceClose`（协议错误、数据损坏等，数据完整性不可信） |

### 代码变更（伪代码）

```cpp
// 修改前
if (ec) {
  self->forceClose(ec);
  return;
}

// 修改后
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

### 行为对比

| 场景 | 修改前 | 修改后 |
|------|--------|--------|
| peer 正常关闭，缓冲区有数据 | **丢弃数据，关闭** | **decode 数据，关闭** |
| peer 异常断开（数据可能损坏） | 立即关闭 | 立即关闭（不变） |
| stop() 取消 async_read | forceClose(abort) | 直接 return（不变） |

### 不涉及修改的部分

- `doWrite()` 中的 `ec` 分支保留不变（写操作失败无法简单重发）
- `stop()` 中的同步 close 不走 ec 分支，无影响
- 其他文件无变更

## 验证

- 确认 `doRead()` 之外无其他读路径存在相同问题（已检查：无）
- `operation_aborted` 分支与现有 `connected_` 检查效果一致，但逻辑更干净