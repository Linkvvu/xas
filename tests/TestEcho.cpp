// tests/TestEcho.cpp
// Integration tests for the xas framework using GoogleTest.
// Each test case spins up an independent TcpServer on a unique OS-assigned
// port, exercises the public API via a synchronous ASIO client, then tears
// the server down cleanly.

#include "xas/xas.h"

#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// ── EchoCodec ────────────────────────────────────────────────────────────────
// Treats every incoming chunk as a complete message and echoes it back
// verbatim.

struct EchoCodec {
  using MessageType = xas::Buffer;

  tl::expected<xas::Buffer, std::error_code> decode(xas::Buffer& buf)
  {
    if (buf.empty())
      return tl::unexpected(
          xas::make_error_code(xas::xas_errc::incomplete_data));
    xas::Buffer msg = std::move(buf);
    buf.clear();
    return msg;
  }

  xas::Buffer encode(const xas::Buffer& msg) { return msg; }
};

// ── Test helpers ─────────────────────────────────────────────────────────────

// Ask the OS to assign a free port by binding to port 0, reading the assigned
// port number, then releasing the acceptor before the caller uses it.
static uint16_t getAvailablePort()
{
  asio::io_context ioc;
  asio::ip::tcp::acceptor a(ioc);
  a.open(asio::ip::tcp::v4());
  a.set_option(asio::ip::tcp::acceptor::reuse_address(true));
  a.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
  uint16_t port = a.local_endpoint().port();
  a.close();
  return port;
}

// Wait for a future with a timeout.  Returns true if the future became ready
// before the deadline, false on timeout.
template <typename T>
static bool
waitFor(std::future<T>& fut,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(3000))
{
  return fut.wait_for(timeout) == std::future_status::ready;
}

// Simple synchronous client: connect → write → read (exact bytes) → close.
// Throws asio::system_error on any network error, which will propagate out of
// the test and be caught by GoogleTest as an unexpected exception.
struct SyncClient {
  asio::io_context ioc;
  asio::ip::tcp::socket sock{ioc};

  void connect(uint16_t port)
  {
    asio::ip::tcp::resolver resolver(ioc);
    auto endpoints = resolver.resolve("127.0.0.1", std::to_string(port));
    asio::connect(sock, endpoints);
  }

  void write(const std::vector<uint8_t>& data)
  {
    asio::write(sock, asio::buffer(data));
  }

  std::vector<uint8_t> read(std::size_t n)
  {
    std::vector<uint8_t> buf(n);
    asio::read(sock, asio::buffer(buf));
    return buf;
  }

  void close()
  {
    std::error_code ec;
    sock.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    sock.close(ec);
  }
};

// ── Test fixture
// ────────────────────────────────────────────────────────────── Each test
// creates its own server with a fresh port; the fixture only provides common
// teardown logic so that the server is always stopped even on failure.

class EchoServer : public ::testing::Test {
protected:
  void TearDown() override
  {
    if (server_) {
      server_->stop();
      server_->wait();
    }
  }

  // Build a server with an EchoCodec already attached and return the handle.
  // The server is stored in server_ so TearDown can clean up.
  xas::CodecHandle<xas::Buffer> makeEchoServer(uint16_t port,
                                               xas::ServerConfig cfg = {})
  {
    server_     = std::make_unique<xas::TcpServer>("0.0.0.0", port, cfg);
    auto codec  = std::make_shared<EchoCodec>();
    auto handle = server_->setCodec(codec);
    return handle;
  }

  std::unique_ptr<xas::TcpServer> server_;
};

// ─────────────────────────────────────────────────────────────────────────────
// 1. ConnectAndEcho
//    Client sends {1,2,3}; server echoes it back; client verifies the payload.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(EchoServer, ConnectAndEcho)
{
  const uint16_t port = getAvailablePort();

  auto handle = makeEchoServer(port);

  // Echo: when a message arrives send it straight back on the same session.
  handle.route(0, [&handle](xas::SessionPtr sess, xas::Buffer msg) {
    handle.sendMsg(sess, msg);
  });

  server_->start();

  SyncClient client;
  ASSERT_NO_THROW(client.connect(port));

  const std::vector<uint8_t> payload = {1, 2, 3};
  ASSERT_NO_THROW(client.write(payload));

  std::vector<uint8_t> reply;
  ASSERT_NO_THROW(reply = client.read(payload.size()));

  EXPECT_EQ(reply, payload);

  client.close();
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. OnConnectCallback
//    The onConnect callback must fire once a client establishes a TCP
//    connection.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(EchoServer, OnConnectCallback)
{
  const uint16_t port = getAvailablePort();

  auto handle = makeEchoServer(port);

  std::promise<xas::SessionPtr> connectedPromise;
  auto connectedFuture = connectedPromise.get_future();

  // Use set_value_at_thread_exit to avoid races when the promise is set from
  // within the ASIO thread pool.
  std::once_flag once;
  server_->onConnect([&](xas::SessionPtr sess) {
    std::call_once(once, [&] { connectedPromise.set_value(sess); });
  });

  server_->start();

  SyncClient client;
  ASSERT_NO_THROW(client.connect(port));

  ASSERT_TRUE(waitFor(connectedFuture))
      << "onConnect callback was not called within 3 s";

  auto sess = connectedFuture.get();
  EXPECT_NE(sess, nullptr);
  EXPECT_TRUE(sess->isConnected());

  client.close();
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. OnDisconnectCallback
//    After the client closes the TCP connection the onDisconnect callback must
//    fire.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(EchoServer, OnDisconnectCallback)
{
  const uint16_t port = getAvailablePort();

  auto handle = makeEchoServer(port);

  std::promise<void> disconnectedPromise;
  auto disconnectedFuture = disconnectedPromise.get_future();

  std::once_flag once;
  server_->onDisconnect([&](xas::SessionPtr /*sess*/, std::error_code /*ec*/) {
    std::call_once(once, [&] { disconnectedPromise.set_value(); });
  });

  server_->start();

  SyncClient client;
  ASSERT_NO_THROW(client.connect(port));

  // Give the server a moment to register the session before we close.
  // (connect callback fires asynchronously; the disconnect handler is
  //  installed before start(), so no race on the callback pointer.)
  client.close();

  ASSERT_TRUE(waitFor(disconnectedFuture))
      << "onDisconnect callback was not called within 3 s";
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. IdleTimeout
//    With idleTimeoutSec = 1 and the onIdle callback calling sess->close(),
//    a connected-but-silent client should have its connection dropped within
//    ~2 seconds.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(EchoServer, IdleTimeout)
{
  const uint16_t port = getAvailablePort();

  xas::ServerConfig cfg;
  cfg.idleTimeoutSec = 1; // 1-second idle timeout

  auto handle = makeEchoServer(port, cfg);

  std::promise<void> idlePromise;
  auto idleFuture = idlePromise.get_future();

  std::once_flag once;
  server_->onIdle([&](xas::SessionPtr sess) {
    // The framework does NOT auto-close; the user callback decides.
    sess->close();
    std::call_once(once, [&] { idlePromise.set_value(); });
  });

  server_->start();

  SyncClient client;
  ASSERT_NO_THROW(client.connect(port));

  // Do NOT send anything – let the idle timer fire.
  // Wait up to 2 × the idle timeout for the callback.
  ASSERT_TRUE(waitFor(idleFuture, std::chrono::milliseconds(2500)))
      << "onIdle callback was not called within 2.5 s";

  // The server should have closed the session; a read on the client side
  // should now fail (EOF or connection-reset).
  // Give the close a moment to propagate through the OS TCP stack.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Attempt a read; we expect it to either throw or return 0 bytes (EOF).
  std::vector<uint8_t> buf(1);
  std::error_code ec;
  std::size_t n = asio::read(client.sock, asio::buffer(buf), ec);
  EXPECT_TRUE(ec || n == 0) << "Expected connection to be closed by the server";

  client.close();
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. MaxConnectionsOverload
//    With maxConnections = 1 the second incoming connection must trigger the
//    onOverload callback.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(EchoServer, MaxConnectionsOverload)
{
  const uint16_t port = getAvailablePort();

  xas::ServerConfig cfg;
  cfg.maxConnections = 1;

  auto handle = makeEchoServer(port, cfg);

  std::promise<void> overloadPromise;
  auto overloadFuture = overloadPromise.get_future();

  std::once_flag once;
  server_->onOverload(
      [&] { std::call_once(once, [&] { overloadPromise.set_value(); }); });

  server_->start();

  // First connection – must succeed and consume the only slot.
  SyncClient client1;
  ASSERT_NO_THROW(client1.connect(port));

  // Wait briefly for the server to register client1 in its session map
  // before we attempt the second connection.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Second connection – the server should accept the TCP handshake at the OS
  // level (the kernel's listen backlog handles that), but immediately reject
  // it at the application level and fire onOverload.
  SyncClient client2;
  ASSERT_NO_THROW(client2.connect(port));

  ASSERT_TRUE(waitFor(overloadFuture))
      << "onOverload callback was not called within 3 s";

  client1.close();
  client2.close();
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. GracefulShutdown
//    After connecting 3 clients and calling stop()+wait(), sessionCount()
//    must return 0.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(EchoServer, GracefulShutdown)
{
  const uint16_t port = getAvailablePort();

  // Use a short shutdown timeout so the test finishes promptly.
  xas::ServerConfig cfg;
  cfg.shutdownTimeoutSec = 2;

  auto handle = makeEchoServer(port, cfg);
  server_->start();

  // Connect 3 clients.
  SyncClient c1, c2, c3;
  ASSERT_NO_THROW(c1.connect(port));
  ASSERT_NO_THROW(c2.connect(port));
  ASSERT_NO_THROW(c3.connect(port));

  // Give the server time to register all three sessions.
  // Poll for up to 1 second rather than sleeping a fixed duration.
  {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (server_->sessionCount() < 3 &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  ASSERT_EQ(server_->sessionCount(), 3u) << "Not all sessions were registered";

  // Initiate graceful shutdown.
  server_->stop();
  server_->wait();

  EXPECT_EQ(server_->sessionCount(), 0u)
      << "sessionCount() should be 0 after stop()+wait()";

  // Suppress TearDown double-stop (server_ is still set but already stopped).
  // TearDown calls stop()+wait() again which is safe due to std::call_once.
}

// Buffer (= std::vector<uint8_t>) 没有 .cmd，使用默认 0
template <>
struct xas::MessageCmd<xas::Buffer> {
  static uint16_t extract(const xas::Buffer& /*msg*/) { return 0; }
};

// ── Request codec
// ───────────────────────────────────────────────────────────── 每条消息带
// uint16_t cmd 头（前2字节为大端序cmd，后面是payload）
struct Request {
  uint16_t cmd;
  xas::Buffer payload;
};

struct RequestCodec {
  using MessageType = Request;

  tl::expected<Request, std::error_code> decode(xas::Buffer& buf)
  {
    if (buf.size() < 2) {
      return tl::unexpected(
          xas::make_error_code(xas::xas_errc::incomplete_data));
    }
    Request req;
    req.cmd =
        (static_cast<uint16_t>(buf[0]) << 8) | static_cast<uint16_t>(buf[1]);
    req.payload = xas::Buffer(buf.begin() + 2, buf.end());
    buf.clear();
    return req;
  }

  xas::Buffer encode(const Request& req)
  {
    xas::Buffer out;
    out.push_back(static_cast<uint8_t>(req.cmd >> 8));
    out.push_back(static_cast<uint8_t>(req.cmd & 0xFF));
    out.insert(out.end(), req.payload.begin(), req.payload.end());
    return out;
  }
};

// ── Route Tests
// ───────────────────────────────────────────────────────────────

class RouteTest : public ::testing::Test {
protected:
  void startServer()
  {
    port_   = getAvailablePort();
    server_ = std::make_unique<xas::TcpServer>("0.0.0.0", port_);
    server_->onConnect(
        [this](xas::SessionPtr s) { sessionCount_.fetch_add(1); });
    server_->onDisconnect([this](xas::SessionPtr, std::error_code) {
      sessionCount_.fetch_sub(1);
    });
    server_->start();
  }

  void stopServer() { server_.reset(); }

  uint16_t port_;
  std::unique_ptr<xas::TcpServer> server_;
  std::atomic<int> sessionCount_{0};
};

#include <spdlog/spdlog.h>
// Test: cmd=1 routed to correct handler
TEST_F(RouteTest, RouteToSpecificHandler)
{
  startServer();

  auto pipeline = server_->setCodec(std::make_shared<RequestCodec>());

  std::atomic<bool> cmd1Called{false};
  pipeline.route(1, [&cmd1Called](xas::SessionPtr x, Request req) {
    spdlog::info("xxxxx");
    EXPECT_EQ(req.cmd, 1);
    cmd1Called.store(true);
  });

  SyncClient client;
  client.connect(port_);

  // Send cmd=1
  Request req{
      1,
      {'h', 'i'}
  };
  client.write(RequestCodec{}.encode(req));

  client.close();
  stopServer();

  EXPECT_TRUE(cmd1Called.load());
}

// Test: cmd=2 NOT routed to cmd=1 handler
TEST_F(RouteTest, NoRouteForDifferentCmd)
{
  startServer();

  auto pipeline = server_->setCodec(std::make_shared<RequestCodec>());

  std::atomic<int> cmd1CallCount{0};
  pipeline.route(1, [&cmd1CallCount](xas::SessionPtr, Request req) {
    spdlog::info("xxxxx");
    cmd1CallCount.fetch_add(1);
  });

  SyncClient client;
  client.connect(port_);

  // Send cmd=2 (not registered)
  Request req{
      2,
      {'t', 'e', 's', 't'}
  };
  client.write(RequestCodec{}.encode(req));

  client.close();
  stopServer();

  // Should not trigger cmd=1 handler
  EXPECT_EQ(cmd1CallCount.load(), 0);
}

// Test: unmatched cmd with no default -> silent drop
TEST_F(RouteTest, UnmatchedCmdSilentDrop)
{
  startServer();

  auto pipeline = server_->setCodec(std::make_shared<RequestCodec>());

  std::atomic<int> totalCalls{0};
  pipeline.route(1, [&totalCalls](xas::SessionPtr, Request) {
    spdlog::info("xxxxx");
    totalCalls.fetch_add(1);
  });
  // No default handler registered

  SyncClient client;
  client.connect(port_);

  // Send cmd=999 (not registered, no default)
  Request req{999, {'x'}};
  client.write(RequestCodec{}.encode(req));

  // Wait a bit for any potential callbacks
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  client.close();
  stopServer();

  EXPECT_EQ(totalCalls.load(), 0); // Should silently drop
}

// Test: unmatched cmd with default -> triggers default
TEST_F(RouteTest, UnmatchedCmdFallsToDefault)
{
  startServer();

  auto pipeline = server_->setCodec(std::make_shared<RequestCodec>());

  std::atomic<int> defaultCallCount{0};
  pipeline.routeDefault([&defaultCallCount](xas::SessionPtr, Request req) {
    spdlog::info("xxxxx");
    EXPECT_EQ(req.cmd, 999);
    defaultCallCount.fetch_add(1);
  });

  SyncClient client;
  client.connect(port_);

  // Send cmd=999 (unmatched, should hit default)
  Request req{999, {'y'}};
  client.write(RequestCodec{}.encode(req));

  // Give server time to process
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  client.close();
  stopServer();

  EXPECT_EQ(defaultCallCount.load(), 1);
}
