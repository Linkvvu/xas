#pragma once

namespace xas {

// 默认 trait：假设消息类型有 .cmd 成员（uint16_t）
template<typename T>
struct MessageCmd {
    static uint16_t extract(const T& msg) { return msg.cmd; }
};

} // namespace xas
