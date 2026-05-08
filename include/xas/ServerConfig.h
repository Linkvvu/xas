#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include "xas/Buffer.h"

namespace spdlog { class logger; }

namespace xas {

struct ServerConfig {
    uint32_t threadCount        = std::thread::hardware_concurrency();
    uint32_t maxConnections     = 10000;
    uint32_t maxReceiveBuffer   = 4 * 1024 * 1024;
    uint32_t idleTimeoutSec     = 60;
    uint32_t shutdownTimeoutSec = 5;
    bool     tcpNoDelay         = true;
    bool     keepAlive          = true;

    std::shared_ptr<spdlog::logger> logger;

    // SessionFactory 完整签名在 TcpServer.h 中定义（需要 ASIO 类型）
    // 此处使用 std::function<SessionPtr(void*)> 作为类型擦除占位，
    // TcpServer 负责将具体 socket 包装后调用。
    using SessionFactory = std::function<SessionPtr(void*)>;
    SessionFactory sessionFactory;
};

} // namespace xas
