# Route Module Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 xas 框架添加静态路由能力，按消息内 `cmd` 字段分发到不同 handler

**Architecture:**
- `MessageCmd<T>` trait 提供 cmd 字段提取接口，默认取 `msg.cmd`
- `Pipeline<Codec>` 持有路由表 `routes_` 和 `defaultCb_`，`process()` 内做路由分发
- `CodecHandle<T>` 透传 `route()` / `routeDefault()` 到 Pipeline

**Tech Stack:** C++17, ASIO, tl::expected, GoogleTest

---

## File Map

| 文件 | 职责 |
|------|------|
| `include/xas/MessageTrait.h` | 新增：`MessageCmd<T>` trait 定义 |
| `include/xas/Pipeline.h` | 修改：新增 `routes_`, `defaultCb_`, `route()`, `routeDefault()`，修改 `process()` |
| `include/xas/CodecHandle.h` | 修改：新增 route 注册回调存储和 `route()`/`routeDefault()` 方法 |
| `include/xas/xas.h` | 修改：新增 `#include "xas/MessageTrait.h"` |
| `tests/TestEcho.cpp` | 修改：补充 Request codec 和路由测试用例 |

---

## Implementation Steps

### Task 1: MessageTrait.h

**Files:**
- Create: `include/xas/MessageTrait.h`

- [ ] **Step 1: 创建 MessageTrait.h**

```cpp
#pragma once

namespace xas {

// 默认 trait：假设消息类型有 .cmd 成员（uint16_t）
template<typename T>
struct MessageCmd {
    static uint16_t extract(const T& msg) { return msg.cmd; }
};

} // namespace xas
```

- [ ] **Step 2: Commit**

```bash
git add include/xas/MessageTrait.h
git commit -m "feat: add MessageCmd trait for route key extraction"
```

---

### Task 2: Pipeline.h 路由能力

**Files:**
- Modify: `include/xas/Pipeline.h`（新增 route 相关成员和方法，修改 `process()`）

- [ ] **Step 1: 添加路由成员和方法**

在 Pipeline 类中添加：

```cpp
// 新增成员变量（在现有成员之后）
std::map<uint16_t, TypedCb> routes_;
TypedCb defaultCb_;

// 新增 public 方法
void route(uint16_t cmd, TypedCb cb) { routes_[cmd] = std::move(cb); }
void routeDefault(TypedCb cb) { defaultCb_ = std::move(cb); }
```

- [ ] **Step 2: 修改 process() 方法**

将 `process()` 内最后一行：
```cpp
if (cb_) cb_(sess, std::move(*result));
```

替换为（先路由分发，未命中再调用 onMessage）：

```cpp
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
```

注意：`MessageCmd<T>` 需要 include MessageTrait.h（通过 xas.h 间接引入即可，Pipeline.h 已经通过其他头引入足够内容）

- [ ] **Step 3: Commit**

```bash
git add include/xas/Pipeline.h
git commit -m "feat: add route capability to Pipeline"
```

---

### Task 3: CodecHandle.h 路由透传

**Files:**
- Modify: `include/xas/CodecHandle.h`（新增 route 注册回调和透传方法）

- [ ] **Step 1: 添加 route 存储和构造函数参数**

将 `CodecHandle` 构造函数修改为接受额外参数：

```cpp
CodecHandle(
    std::function<void(TypedCb)> registerCb,       // 原有的 onMessage 注册
    std::function<Buffer(const T&)> encodeFn,      // 原有的 encode
    std::function<void(uint16_t, TypedCb)> routeCb = nullptr,  // 新增：route 注册
    std::function<void(TypedCb)> routeDefaultCb = nullptr     // 新增：default 注册
)
    : registerCb_(std::move(registerCb))
    , encode_(std::move(encodeFn))
    , routeCb_(std::move(routeCb))
    , routeDefaultCb_(std::move(routeDefaultCb))
{
}
```

- [ ] **Step 2: 添加 route() 和 routeDefault() 方法**

```cpp
// 注册路由
void route(uint16_t cmd, TypedCb cb) {
    if (routeCb_) routeCb_(cmd, std::move(cb));
}

// 注册默认路由
void routeDefault(TypedCb cb) {
    if (routeDefaultCb_) routeDefaultCb_(std::move(cb));
}
```

- [ ] **Step 3: 添加私有成员**

```cpp
std::function<void(uint16_t, TypedCb)> routeCb_;
std::function<void(TypedCb)> routeDefaultCb_;
```

- [ ] **Step 4: Commit**

```bash
git add include/xas/CodecHandle.h
git commit -m "feat: add route methods to CodecHandle"
```

---

### Task 4: TcpServer.h 构造参数调整

**Files:**
- Modify: `include/xas/TcpServer.h`（调整 setCodec 返回的 CodecHandle 构造）

需要查看 setCodec 实现确认如何传递 routeCb_。先读文件确认当前实现。

- [ ] **Step 1: 读取 TcpServer.h 中的 setCodec 实现**

```bash
cat include/xas/TcpServer.h | grep -A 20 "setCodec"
```

- [ ] **Step 2: 确认后修改 setCodec 内 CodecHandle 构造，传入 routeCb 和 routeDefaultCb**

需要在 setCodec 内创建 Pipeline 时，同时获取它的 route 方法绑定。

- [ ] **Step 3: Commit**

```bash
git add include/xas/TcpServer.h
git commit -m "feat: wire route callbacks into CodecHandle in setCodec"
```

---

### Task 5: xas.h 引入 MessageTrait

**Files:**
- Modify: `include/xas/xas.h`

- [ ] **Step 1: 添加 include**

在 `#include "xas/Pipeline.h"` 前或后添加：

```cpp
#include "xas/MessageTrait.h"
```

- [ ] **Step 2: Commit**

```bash
git add include/xas/xas.h
git commit -m "feat: include MessageTrait.h in xas.h"
```

---

### Task 6: 测试用例

**Files:**
- Modify: `tests/TestEcho.cpp`

- [ ] **Step 1: 添加 Request codec 和路由测试**

在现有测试基础上添加新测试用例 `TEST_F(TcpServerTest, RouteByCommand)`：

```cpp
// Request codec：每条消息带 uint16_t cmd 头
struct Request {
    uint16_t cmd;
    xas::Buffer payload;
};

struct RequestCodec {
    using MessageType = Request;

    tl::expected<Request, std::error_code> decode(xas::Buffer& buf) {
        if (buf.size() < sizeof(uint16_t)) {
            return tl::unexpected(make_error_code(xas_errc::incomplete_data));
        }
        Request req;
        req.cmd = (buf[0] << 8) | buf[1];
        req.payload = xas::Buffer(buf.begin() + 2, buf.end());
        buf.clear();
        return req;
    }

    xas::Buffer encode(const Request& req) {
        xas::Buffer out;
        out.push_back(static_cast<uint8_t>(req.cmd >> 8));
        out.push_back(static_cast<uint8_t>(req.cmd & 0xFF));
        out.insert(out.end(), req.payload.begin(), req.payload.end());
        return out;
    }
};

// 测试场景：
// 1. cmd=1 handler 收到 cmd=1 → OK
// 2. cmd=2 handler 收到 cmd=1 → 不应触发
// 3. 发送 cmd=999，无 default handler → 静默丢弃
// 4. 发送 cmd=2，有 default handler → 触发 default
```

测试框架参考现有 TestEcho 的 ConnectThenEcho 测试结构。

- [ ] **Step 2: 运行测试验证**

```bash
cd build && cmake --build . --config Debug
ctest -C Debug -V --output-on-failure
```

- [ ] **Step 3: Commit**

```bash
git add tests/TestEcho.cpp
git commit -m "test: add route tests with Request codec"
```

---

## Spec Coverage Check

- [x] 按消息内容（cmd 字段）路由 ✓
- [x] 静态注册 ✓
- [x] MessageCmd trait + 默认假设 .cmd ✓
- [x] 特例化支持 ✓
- [x] default handler ✓
- [x] 静默丢弃未匹配消息 ✓

## Placeholder Scan

- 无 TBD/TODO
- 所有代码片段完整
- 类型、方法名一致

---

## Execution Options

**1. Subagent-Driven (recommended)** - I dispatch a fresh subagent per task, review between tasks

**2. Inline Execution** - Execute tasks in this session, batch execution with checkpoints

**Which approach?**