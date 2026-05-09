# Codec Error Reporting — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Integrate `tl::expected` into xas codec pipeline to distinguish incomplete data (retry) from invalid format (triggers onError then forceClose).

**Architecture:** Add `xas_errc` enum + `xas_category` to `xas.h`, change `Pipeline::process()` to use `tl::expected` return type, wire `errorCb_` from TcpServer into Pipeline.

**Tech Stack:** C++17, tl-expected (header-only), ASIO, spdlog

---

## File Overview

| File | Responsibility |
|------|----------------|
| `include/xas/xas.h` | Add `xas_errc`, `xas_category`, `make_error_code()` |
| `include/xas/Pipeline.h` | Add `errorCb_` member, `setErrorCb()`, update `process()` |
| `include/xas/TcpServer.h` | Wire `errorCb_` into pipeline in `setCodec()` |
| `include/xas/TcpSession.h` | Add `template<typename> friend class Pipeline;` for `forceClose` access |
| `vcpkg.json` | Add `tl-expected` dependency |
| `examples/echo/main.cpp` | Update EchoCodec to new decode signature |
| `tests/TestEcho.cpp` | Update EchoCodec to new decode signature |

---

## Dependency Setup

### Task 1: Add tl-expected to vcpkg.json

**Files:**
- Modify: `vcpkg.json:4`

- [ ] **Step 1: Add tl-expected to vcpkg.json**

```json
{
  "name": "xas",
  "version": "0.1.0",
  "dependencies": [
    "asio",
    "spdlog",
    "gtest",
    "tl-expected"
  ]
}
```

- [ ] **Step 2: Commit**

```bash
git add vcpkg.json && git commit -m "deps: add tl-expected"
```

---

## Core Infrastructure

### Task 2: Add xas_errc and xas_category to xas.h

**Files:**
- Modify: `include/xas/xas.h:1-7`
- Test: `tests/TestEcho.cpp` (existing tests verify no breaking changes)

- [ ] **Step 1: Add xas_errc enum, xas_category, make_error_code() to xas.h**

```cpp
#pragma once
#include "xas/Buffer.h"
#include "xas/CodecHandle.h"
#include "xas/Pipeline.h"
#include "xas/ServerConfig.h"
#include "xas/TcpServer.h"
#include "xas/TcpSession.h"

#include <system_error>

namespace xas {

enum class xas_errc {
  incomplete_data = 1,
  invalid_format = 2,
};

class xas_category : public std::error_category {
public:
  const char* name() const noexcept override { return "xas"; }
  std::string message(int ev) const override {
    switch (static_cast<xas_errc>(ev)) {
      case xas_errc::incomplete_data: return "incomplete data";
      case xas_errc::invalid_format:  return "invalid format";
    }
    return "unknown xas error";
  }
};

inline std::error_code make_error_code(xas_errc e) {
  static xas_category instance;
  return std::error_code(static_cast<int>(e), instance);
}

} // namespace xas
```

- [ ] **Step 2: Verify build compiles**

Run: `cmake --build build 2>&1 | head -50`

- [ ] **Step 3: Commit**

```bash
git add include/xas/xas.h && git commit -m "feat: add xas_errc enum and xas_category"
```

---

## Pipeline Integration

### Task 3: Update Pipeline.h

**Files:**
- Modify: `include/xas/Pipeline.h:1-42`
- Modify: `include/xas/TcpSession.h:14-20` (add Pipeline friend for forceClose access)

- [ ] **Step 1: Update Pipeline.h — add errorCb_, setErrorCb(), new process()**

```cpp
#pragma once
#include "xas/Buffer.h"

#include <functional>
#include <memory>
#include <optional>
#include <tl/expected.hpp>

namespace xas {

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
      if (cb_) cb_(sess, std::move(*result));
    }
  }

  Buffer encode(const T& msg) { return codec_->encode(msg); }

private:
  std::shared_ptr<Codec> codec_;
  TypedCb cb_;
  std::function<void(SessionPtr, std::error_code)> errorCb_;
};

} // namespace xas
```

- [ ] **Step 2: Verify build compiles**

Run: `cmake --build build 2>&1 | head -50`

- [ ] **Step 3: Commit**

```bash
git add include/xas/Pipeline.h && git commit -m "feat: Pipeline uses tl::expected, invalid_format triggers forceClose"
```

---

### Task 4: Wire errorCb_ from TcpServer into Pipeline

**Files:**
- Modify: `include/xas/TcpServer.h:39-47`

- [ ] **Step 1: Update setCodec() to pass errorCb_ to pipeline**

```cpp
template <typename Codec>
CodecHandle<typename Codec::MessageType>
setCodec(std::shared_ptr<Codec> codec)
{
  using T       = typename Codec::MessageType;
  auto pipeline = std::make_shared<Pipeline<Codec>>(std::move(codec));
  rawCb_ = [pipeline](SessionPtr s, Buffer& b) { pipeline->process(s, b); };
  pipeline->setErrorCb(errorCb_);
  return CodecHandle<T>(
      [pipeline](std::function<void(SessionPtr, T)> cb) {
        pipeline->setMessageCb(std::move(cb));
      },
      [pipeline](const T& msg) { return pipeline->encode(msg); });
}
```

- [ ] **Step 2: Verify build compiles**

Run: `cmake --build build 2>&1 | head -50`

- [ ] **Step 3: Commit**

```bash
git add include/xas/TcpServer.h && git commit -m "feat: wire errorCb_ into pipeline in setCodec()"
```

---

## Codec Migration

### Task 5: Migrate EchoCodec in examples/echo/main.cpp

**Files:**
- Modify: `examples/echo/main.cpp:10-23`

- [ ] **Step 1: Update EchoCodec to use tl::expected**

```cpp
// ---------------------------------------------------------------------------
// EchoCodec — satisfy the xas Codec duck-typing contract.
// Strategy: accumulate nothing; every chunk of received bytes is a message.
// ---------------------------------------------------------------------------
struct EchoCodec {
  using MessageType = xas::Buffer;

  tl::expected<xas::Buffer, std::error_code> decode(xas::Buffer& buf) {
    if (buf.empty())
      return tl::unexpected(make_error_code(xas_errc::incomplete_data));
    xas::Buffer msg = std::move(buf);
    buf.clear();
    return msg;
  }

  xas::Buffer encode(const xas::Buffer& msg) { return msg; }
};
```

- [ ] **Step 2: Verify build compiles**

Run: `cmake --build build 2>&1 | head -50`

- [ ] **Step 3: Commit**

```bash
git add examples/echo/main.cpp && git commit -m "feat: migrate EchoCodec to tl::expected"
```

---

### Task 6: Migrate EchoCodec in tests/TestEcho.cpp

**Files:**
- Modify: `tests/TestEcho.cpp:24-37`

- [ ] **Step 1: Update EchoCodec to use tl::expected**

```cpp
struct EchoCodec {
  using MessageType = xas::Buffer;

  tl::expected<xas::Buffer, std::error_code> decode(xas::Buffer& buf) {
    if (buf.empty())
      return tl::unexpected(make_error_code(xas_errc::incomplete_data));
    xas::Buffer msg = std::move(buf);
    buf.clear();
    return msg;
  }

  xas::Buffer encode(const xas::Buffer& msg) { return msg; }
};
```

- [ ] **Step 2: Run tests to verify everything passes**

Run: `cmake --build build --target testecho 2>&1` (or whatever test target exists)
Expected: All existing tests pass

- [ ] **Step 3: Commit**

```bash
git add tests/TestEcho.cpp && git commit -m "test: migrate TestEcho EchoCodec to tl::expected"
```

---

## Verification

- [ ] **Final build check:** `cmake --build build && ctest --output-on-failure`
- [ ] **Run full test suite:** All tests pass