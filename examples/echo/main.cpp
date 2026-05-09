#include "xas/xas.h"

#include <spdlog/spdlog.h>

// ---------------------------------------------------------------------------
// EchoCodec — satisfy the xas Codec duck-typing contract.
// Strategy: accumulate nothing; every chunk of received bytes is a message.
// ---------------------------------------------------------------------------
struct EchoCodec {
  using MessageType = xas::Buffer;

  tl::expected<xas::Buffer, std::error_code> decode(xas::Buffer& buf)
  {
    if (buf.empty())
      return tl::unexpected(xas::make_error_code(xas::xas_errc::incomplete_data));
    xas::Buffer msg = std::move(buf);
    buf.clear();
    return msg;
  }

  xas::Buffer encode(const xas::Buffer& msg) { return msg; }
};

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main()
{
  xas::ServerConfig config;
  config.threadCount    = 2;
  config.idleTimeoutSec = 30;

  xas::TcpServer server("0.0.0.0", 8080, config);

  server.onConnect([](xas::SessionPtr sess) {
    spdlog::info("client connected  id={} addr={}:{}",
                 sess->id(),
                 sess->remoteAddress(),
                 sess->remotePort());
  });

  server.onDisconnect([](xas::SessionPtr sess, std::error_code ec) {
    spdlog::info("client disconnected id={} reason=[{}] {}",
                 sess->id(),
                 ec.value(),
                 ec.message());
  });

  server.onError([](xas::SessionPtr sess, std::error_code ec) {
    spdlog::warn("session error id={} [{}] {}",
                 sess->id(),
                 ec.value(),
                 ec.message());
  });

  server.onIdle([](xas::SessionPtr sess) {
    spdlog::info("idle timeout — closing session id={}", sess->id());
    sess->close();
  });

  auto pipeline = server.setCodec(std::make_shared<EchoCodec>());

  pipeline.onMessage([&pipeline](xas::SessionPtr sess, xas::Buffer msg) {
    spdlog::info("echo {} bytes to id={}", msg.size(), sess->id());
    pipeline.sendMsg(sess, msg);
  });

  spdlog::info("echo server listening on 0.0.0.0:8080");
  server.run(); // blocks until stopped
}
