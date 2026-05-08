#pragma once
#include "xas/Buffer.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

namespace spdlog {
class logger;
}

namespace xas {

struct ServerConfig {
  uint32_t threadCount        = std::thread::hardware_concurrency();
  uint32_t maxConnections     = 10000;
  uint32_t maxReceiveBuffer   = 4 * 1024 * 1024;
  uint32_t idleTimeoutSec     = 60;
  uint32_t shutdownTimeoutSec = 5;
  bool tcpNoDelay             = true;
  bool keepAlive              = true;

  std::shared_ptr<spdlog::logger> logger;
};

} // namespace xas
