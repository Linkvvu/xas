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
