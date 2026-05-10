# Route Module Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add static route capability to xas framework - dispatch messages to different handlers based on internal `cmd` field

**Architecture:**
- `MessageCmd<T>` trait provides cmd field extraction, defaults to `msg.cmd`
- `Pipeline<Codec>` holds `routes_` map and `defaultCb_`, dispatches in `process()`
- `CodecHandle<T>` forwards `route()` / `routeDefault()` to Pipeline
- `onMessage` is REMOVED - all message handling must go through `route()` or `routeDefault()`

**Tech Stack:** C++17, ASIO, tl::expected, GoogleTest

---

## File Map

| File | Responsibility |
|------|----------------|
| `include/xas/MessageTrait.h` | New: `MessageCmd<T>` trait |
| `include/xas/Pipeline.h` | Modify: add `routes_`, `defaultCb_`, `route()`, `routeDefault()`; remove `cb_`/`setMessageCb`; modify `process()` |
| `include/xas/CodecHandle.h` | Modify: add route storage and `route()`/`routeDefault()` methods; remove `onMessage` |
| `include/xas/xas.h` | Done: already includes `MessageTrait.h` |
| `tests/TestEcho.cpp` | Modify: add Request codec and route tests |

---

## Implementation Steps

### Task 1: MessageTrait.h

**Files:**
- Create: `include/xas/MessageTrait.h`

- [ ] **Step 1: Create MessageTrait.h**

```cpp
#pragma once

namespace xas {

// Default trait: assumes message type has .cmd member (uint16_t)
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

### Task 2: Pipeline.h - remove cb_, add route

**Files:**
- Modify: `include/xas/Pipeline.h`

**Current state (already has route logic with cb_ fallback):**
```cpp
void setMessageCb(TypedCb cb) { cb_ = std::move(cb); }
void route(uint16_t cmd, TypedCb cb) { routes_[cmd] = std::move(cb); }
void routeDefault(TypedCb cb) { defaultCb_ = std::move(cb); }
```

**Target state: REMOVE setMessageCb and cb_ entirely**

- [ ] **Step 1: Remove `setMessageCb` and `cb_`**

Remove from class:
```cpp
void setMessageCb(TypedCb cb) { cb_ = std::move(cb); }  // REMOVE
```

Remove from private members:
```cpp
TypedCb cb_;  // REMOVE
```

- [ ] **Step 2: Modify process() - remove cb_ fallback**

Replace the current route dispatch logic:
```cpp
// OLD (has cb_ fallback):
if (cb_) {
    uint16_t key = MessageCmd<T>::extract(*result);
    auto it = routes_.find(key);
    if (it != routes_.end()) {
        it->second(sess, std::move(*result));
    } else if (defaultCb_) {
        defaultCb_(sess, std::move(*result));
    } else {
        cb_(sess, std::move(*result));  // cb_ fallback - REMOVE
    }
}

// NEW (no cb_ fallback):
uint16_t key = MessageCmd<T>::extract(*result);
auto it = routes_.find(key);
if (it != routes_.end()) {
    it->second(sess, std::move(*result));
} else if (defaultCb_) {
    defaultCb_(sess, std::move(*result));
}
// else: silent drop
```

Note: `process()` now always does route dispatch - no cb_ needed.

- [ ] **Step 3: Commit**

```bash
git add include/xas/Pipeline.h
git commit -m "feat: remove cb_ from Pipeline, route is the only dispatch path"
```

---

### Task 3: CodecHandle.h - remove onMessage, add route

**Files:**
- Modify: `include/xas/CodecHandle.h`

**Current state (has onMessage):**
```cpp
void onMessage(TypedCb cb) { registerCb_(std::move(cb)); }
```

**Target state: REMOVE onMessage, add route() and routeDefault()**

- [ ] **Step 1: Add route storage to constructor**

Current constructor takes `registerCb` for onMessage. Modify:

```cpp
CodecHandle(
    std::function<void(TypedCb)> registerCb,       // REMOVE - was for onMessage
    std::function<Buffer(const T&)> encodeFn,      // keep
    std::function<void(uint16_t, TypedCb)> routeCb = nullptr,  // NEW
    std::function<void(TypedCb)> routeDefaultCb = nullptr     // NEW
)
    : registerCb_(std::move(registerCb))           // REMOVE from init list
    , encode_(std::move(encodeFn))
    , routeCb_(std::move(routeCb))
    , routeDefaultCb_(std::move(routeDefaultCb))
{
}
```

- [ ] **Step 2: Remove onMessage, add route() and routeDefault()**

Remove:
```cpp
void onMessage(TypedCb cb) { registerCb_(std::move(cb)); }  // REMOVE
```

Add:
```cpp
void route(uint16_t cmd, TypedCb cb) {
    if (routeCb_) routeCb_(cmd, std::move(cb));
}

void routeDefault(TypedCb cb) {
    if (routeDefaultCb_) routeDefaultCb_(std::move(cb));
}
```

- [ ] **Step 3: Remove registerCb_ and add route members**

Remove from private:
```cpp
std::function<void(TypedCb)> registerCb_;  // REMOVE
```

Add to private:
```cpp
std::function<void(uint16_t, TypedCb)> routeCb_;
std::function<void(TypedCb)> routeDefaultCb_;
```

- [ ] **Step 4: Commit**

```bash
git add include/xas/CodecHandle.h
git commit -m "feat: remove onMessage from CodecHandle, add route methods"
```

---

### Task 4: TcpServer.h - wire route callbacks

**Files:**
- Modify: `include/xas/TcpServer.h`

- [ ] **Step 1: Read current setCodec implementation**

```bash
cat include/xas/TcpServer.h
```

- [ ] **Step 2: Modify setCodec to pass routeCb and routeDefaultCb to CodecHandle**

Need to bind Pipeline's route methods when constructing CodecHandle:

```cpp
template<typename Codec>
CodecHandle<typename Codec::MessageType> setCodec(std::shared_ptr<Codec> codec) {
    auto pipeline = std::make_shared<Pipeline<Codec>>(std::move(codec));

    return CodecHandle<typename Codec::MessageType>(
        [pipeline](auto&& cb) { pipeline->setMessageCb(std::move(cb)); },  // REMOVE THIS
        [pipeline](const auto& msg) { return pipeline->encode(msg); },
        [pipeline](uint16_t cmd, auto&& cb) { pipeline->route(cmd, std::move(cb)); },      // NEW
        [pipeline](auto&& cb) { pipeline->routeDefault(std::move(cb)); }   // NEW
    );
}
```

- [ ] **Step 3: Commit**

```bash
git add include/xas/TcpServer.h
git commit -m "feat: wire route callbacks in setCodec"
```

---

### Task 5: xas.h (ALREADY DONE)

- [x] Already includes `#include "xas/MessageTrait.h"`

---

### Task 6: Tests

**Files:**
- Modify: `tests/TestEcho.cpp`

- [ ] **Step 1: Add Request codec**

```cpp
// Request codec: each message has uint16_t cmd header
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
```

- [ ] **Step 2: Add route test cases**

```cpp
TEST_F(TcpServerTest, RouteByCommand) {
    auto port = getAvailablePort();
    xas::TcpServer server("127.0.0.1", port);

    std::atomic<int> cmd1Count{0};
    std::atomic<int> cmd2Count{0};
    std::atomic<int> defaultCount{0};

    auto pipeline = server.setCodec(std::make_shared<RequestCodec>());

    pipeline.route(1, [&cmd1Count](xas::SessionPtr, Request req) {
        cmd1Count++;
    });

    pipeline.route(2, [&cmd2Count](xas::SessionPtr, Request req) {
        cmd2Count++;
    });

    pipeline.routeDefault([&defaultCount](xas::SessionPtr, Request req) {
        defaultCount++;
    });

    server.start();

    // Send cmd=1
    SyncClient c1;
    c1.connect(port);
    Request r1{1, {'a'}};
    c1.write(pipeline.sendMsg(r1));  // Need helper or use encode directly
    c1.close();

    // Send cmd=999 (hits default)
    SyncClient c2;
    c2.connect(port);
    Request r2{999, {'b'}};
    c2.write(pipeline.sendMsg(r2));
    c2.close();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    server.stop();

    EXPECT_EQ(cmd1Count, 1);
    EXPECT_EQ(cmd2Count, 0);
    EXPECT_EQ(defaultCount, 1);
}
```

Note: `sendMsg` needs to be accessible - may need to expose `pipeline.encode()` or use `server.setCodec()` return differently.

- [ ] **Step 3: Run tests**

```bash
cd build && cmake --build . --config Debug
ctest -C Debug -V --output-on-failure
```

- [ ] **Step 4: Commit**

```bash
git add tests/TestEcho.cpp
git commit -m "test: add route tests with RequestCodec"
```

---

## Spec Coverage Check

- [x] Route by message content (cmd field) - `MessageCmd<T>` trait
- [x] Static registration - `route()` at startup
- [x] `MessageCmd` trait + default `.cmd` member assumption
- [x] Specialization support via template specialization
- [x] `defaultCb_` as catch-all
- [x] Silent drop for unmatched cmd without default
- [x] `onMessage` REMOVED - route is only dispatch path

## Placeholder Scan

- No TBD/TODO
- All code complete
- Types and method names consistent

---

## Execution Options

**1. Subagent-Driven (recommended)** - I dispatch a fresh subagent per task, review between tasks

**2. Inline Execution** - Execute tasks in this session, batch execution with checkpoints

**Which approach?**
