# Route Module Design

> Date: 2026-05-10 | Status: Draft

## 背景

当前 xas 框架的 `CodecHandle<T>` 仅支持单一 `onMessage` 回调。用户业务需要根据消息内部字段（如 command ID）分发到不同 handler。本设计为框架添加**静态路由**能力。

## 设计决策

- **路由维度**: 按消息内容字段（`uint16_t cmd`）分发
- **注册方式**: 静态注册（启动时配置，运行期不变）
- **方案**: 方案 1 - 简单 Map 路由（最小改动，与现有架构兼容）

## 架构

```
CodecHandle<T>
  └─► Pipeline<T>
       ├─► std::map<uint16_t, TypedCb> routes_
       ├─► TypedCb defaultCb_
       ├─► route(cmd, cb)
       └─► routeDefault(cb)

process() 修改:
  while (msg = codec->decode(buf)) {
      auto it = routes_.find(msg->cmd);
      if (it != routes_.end())
          it->second(sess, *msg);
      else if (defaultCb_)
          defaultCb_(sess, *msg);
      // else: 静默丢弃
  }
```

## 用户接口

```cpp
auto pipeline = server.setCodec(std::make_shared<MyCodec>());

pipeline.route(1, [](xas::SessionPtr sess, Request req) {
    // cmd=1: login
});

pipeline.route(2, [](xas::SessionPtr sess, Request req) {
    // cmd=2: send message
});

pipeline.route(3, [](xas::SessionPtr sess, Request req) {
    // cmd=3: query user
});

pipeline.routeDefault([](xas::SessionPtr sess, Request req) {
    // unknown cmd handler
});
```

## 行为约定

| 场景 | 行为 |
|------|------|
| cmd 命中 | 调用对应 handler |
| cmd 未命中，有 default | 调用 default handler |
| cmd 未命中，无 default | 静默丢弃该消息 |

## cmd 字段提取机制

使用 `MessageCmd<T>` trait 提取 `cmd`，默认假设消息类型有 `.cmd` 成员：

```cpp
// include/xas/MessageTrait.h
namespace xas {

template<typename T>
struct MessageCmd {
    static uint16_t extract(const T& msg) { return msg.cmd; }
};

} // namespace xas
```

Pipeline 内使用：
```cpp
uint16_t key = MessageCmd<T>::extract(*msg);
```

**特例化示例**（消息类型无 `.cmd` 字段时）：

```cpp
struct RawPacket {
    uint8_t  type;   // 用 type 字段
    uint32_t payload;
};

template<>
struct xas::MessageCmd<RawPacket> {
    static uint16_t extract(const RawPacket& msg) { return msg.type; }
};
```

## 文件变更

| 文件 | 变更 |
|------|------|
| `include/xas/MessageTrait.h` | 新增 `MessageCmd<T>` trait |
| `include/xas/Pipeline.h` | 新增 `routes_`, `defaultCb_`, `route()`, `routeDefault()`，修改 `process()` |
| `include/xas/CodecHandle.h` | 透传 `route()` / `routeDefault()` 到 Pipeline |
| `include/xas/xas.h` | 新增 `#include "xas/MessageTrait.h"` |
| `DESIGN.md` | 新增 Route 模块章节 |
| `tests/TestEcho.cpp` | 补充路由测试 |

## 测试用例

1. `route(1, handler)` 注册后，cmd=1 消息触发 handler
2. cmd 未命中且无 default → 无崩溃，静默丢弃
3. cmd 未命中但有 default → 调用 default
4. `routeDefault` 覆盖后生效
5. 消息无 `.cmd` 字段 → 通过 `MessageCmd` 特例化提取
