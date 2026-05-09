# xas Codec Error Reporting — Design Spec

## Status

Draft — awaiting approval

## Background

当前 `Pipeline::process()` 调用 `codec_->decode()` 返回 `std::optional<T>`，只能区分"成功"和"不完整"两种状态，无法表达"无效格式"错误。

框架已在 DESIGN.md 中定义了 `onError(sess, ec)` 回调（第 335 行），但 `Pipeline` 实现从未触发它。

本设计引入 `tl::expected` + 自定义 `std::error_code` 枚举，使 codec 能够表达三种解码状态，并在 `Pipeline` 中正确触发 `onError`。

---

## Error Code Design

### 1. xas_errc 枚举

所有 xas 框架错误码集中在一个枚举，位于 `include/xas/error.h`：

```cpp
namespace xas {
enum class xas_errc {
  incomplete_data = 1, // 数据不完整，buffer 保留，下次再来
  invalid_format = 2,   // 无效格式，触发 onError，用户决定如何处理
};
} // namespace xas
```

### 2. xas_category

单一 error category 实现，接入 `std::error_code` 体系：

```cpp
namespace xas {

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

使用示例：
```cpp
return tl::unexpected(make_error_code(xas_errc::incomplete_data));
return tl::unexpected(make_error_code(xas_errc::invalid_format));
```

---

## Codec Interface Change

### 旧接口

```cpp
struct Codec {
  using MessageType = ...;
  std::optional<T> decode(Buffer& buf);
};
```

### 新接口

```cpp
#include <tl/expected.hpp>

struct Codec {
  using MessageType = ...;
  tl::expected<T, std::error_code> decode(Buffer& buf);
};
```

**返回语义：**
| 返回值 | 含义 |
|--------|------|
| `return msg;` | 解码成功，返回消息 |
| `return tl::unexpected(make_error_code(xas_errc::incomplete_data));` | 数据不完整，buffer 保留，退出本轮解码循环 |
| `return tl::unexpected(make_error_code(xas_errc::invalid_format));` | 无效格式，触发 `onError` |

---

## Pipeline::process Implementation

```cpp
// Pipeline.h 新增成员
std::function<void(SessionPtr, std::error_code)> errorCb_;;

public:
void setErrorCb(std::function<void(SessionPtr, std::error_code)> cb) {
  errorCb_ = std::move(cb);
}

void process(SessionPtr sess, Buffer& buf)
{
  while (true) {
    auto result = codec_->decode(buf);
    if (!result) {
      const auto& ec = result.error();
      if (ec == make_error_code(xas_errc::incomplete_data)) {
        // 数据不完整，退出循环，等更多数据
        return;
      }
      // 无效格式 → 先触发 onError（供用户记录），再强制关闭 session
      if (errorCb_) errorCb_(sess, ec);
      sess->forceClose(ec);
      return;
    }
    if (cb_) cb_(sess, std::move(*result));
  }
}
```

---

## Existing Codec Migration

### EchoCodec（examples/echo/main.cpp, tests/TestEcho.cpp）

**旧：**
```cpp
std::optional<xas::Buffer> decode(xas::Buffer& buf) {
  if (buf.empty()) return std::nullopt;
  xas::Buffer msg = std::move(buf);
  buf.clear();
  return msg;
}
```

**新：**
```cpp
#include <tl/expected.hpp>

tl::expected<xas::Buffer, std::error_code> decode(xas::Buffer& buf) {
  if (buf.empty())
    return tl::unexpected(make_error_code(xas_errc::incomplete_data));
  xas::Buffer msg = std::move(buf);
  buf.clear();
  return msg;
}
```

---

## Files to Modify

| File | Change |
|------|--------|
| `include/xas/error.h` | 新建 — `xas_errc` 枚举、`xas_category`、`make_error_code(xas_errc)` |
| `include/xas/xas.h` | 改为 `#include "xas/error.h"` 再包含其他模块 |
| `include/xas/Pipeline.h` | `process()` 改为使用 `tl::expected`，`invalid_format` 时触发 onError 后调用 `sess->forceClose(ec)` |
| `include/xas/CodecHandle.h` | 无需修改（仅传递 typed callback） |
| `include/xas/TcpServer.h` | 添加 `<optional>`（移除 Pipeline.h 的 transitive include 后需要） |
| `include/xas/TcpSession.h` | 添加 `template<typename> friend class Pipeline;`（让 Pipeline 访问 forceClose） |
| `examples/echo/main.cpp` | EchoCodec 迁移到新接口 |
| `tests/TestEcho.cpp` | EchoCodec 迁移到新接口 |

---

## Dependency

- `tl/expected` — header-only，`#include <tl/expected.hpp>`
- 需要 CMake/`vcpkg.json` 中引入 `tl-expected` 包（若尚未引入）

---

## Design Rationale

1. **统一错误分类**：所有 xas 错误码在 `xas_category` 下，框架与 codec 错误均可扩展
2. **不完整 vs 无效格式**：通过 `xas_errc::incomplete_data` 和 `xas_errc::invalid_format` 明确区分
3. **用户决定权**：invalid_format 触发 `onError`，用户可选择 close session、记录日志或做其他处理
4. **向后兼容**：codec 接口签名变化，但用户层回调（onMessage、onError）不变