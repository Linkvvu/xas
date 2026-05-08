#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace xas {

using Buffer = std::vector<uint8_t>;
class TcpSession;
using SessionPtr  = std::shared_ptr<TcpSession>;
using WeakSession = std::weak_ptr<TcpSession>;

} // namespace xas
