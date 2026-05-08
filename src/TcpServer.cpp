#include "xas/TcpServer.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace xas {

// ── Constructor ───────────────────────────────────────────────────────────
TcpServer::TcpServer(std::string host, uint16_t port, ServerConfig config)
    : host_(std::move(host))
    , port_(port)
    , config_(std::move(config))
    , ioc_()
    , acceptor_(ioc_)
    , shutdownTimer_(ioc_)
{
  // Default logger setup
  if (!config_.logger) {
    auto existing = spdlog::get("xas");
    if (existing) {
      config_.logger = existing;
    } else {
      config_.logger = spdlog::stdout_color_mt("xas");
    }
  }

  // work guard keeps ioc_ alive until explicitly released
  workGuard_ = std::make_unique<
      asio::executor_work_guard<asio::io_context::executor_type>>(
      ioc_.get_executor());

  // Resolve, open, bind and listen
  asio::ip::tcp::resolver resolver(ioc_);
  auto endpoints = resolver.resolve(host_, std::to_string(port_));
  auto endpoint  = endpoints.begin()->endpoint();

  acceptor_.open(endpoint.protocol());
  acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
  acceptor_.bind(endpoint);
  acceptor_.listen();

  config_.logger->info("TcpServer bound to {}:{}", host_, port_);
}

// ── Destructor ────────────────────────────────────────────────────────────
TcpServer::~TcpServer()
{
  stop();
  wait();
}

// ── Lifecycle callbacks ───────────────────────────────────────────────────
void TcpServer::onConnect(std::function<void(SessionPtr)> cb)
{
  connectCb_ = std::move(cb);
}

void TcpServer::onDisconnect(
    std::function<void(SessionPtr, std::error_code)> cb)
{
  disconnectCb_ = std::move(cb);
}

void TcpServer::onError(std::function<void(SessionPtr, std::error_code)> cb)
{
  errorCb_ = std::move(cb);
}

void TcpServer::onIdle(std::function<void(SessionPtr)> cb)
{
  idleCb_ = std::move(cb);
}

void TcpServer::onOverload(std::function<void()> cb)
{
  overloadCb_ = std::move(cb);
}

// ── start / run / wait / stop ─────────────────────────────────────────────
bool TcpServer::start()
{
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true))
    return false;

  doAccept();

  uint32_t n = config_.threadCount > 0 ? config_.threadCount : 1;
  threads_.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    threads_.emplace_back([this] { ioc_.run(); });
  }

  config_.logger->info("TcpServer started with {} thread(s)", n);
  return true;
}

bool TcpServer::run()
{
  if (!start())
    return false;
  wait();
  return true;
}

void TcpServer::wait()
{
  if (!started_.load()) {
    config_.logger->warn("wait() called before start() — no-op");
    return;
  }

  std::call_once(waitOnce_, [this] {
    for (auto& t : threads_) {
      if (t.joinable())
        t.join();
    }
  });
}

void TcpServer::stop()
{
  if (!started_.load()) {
    config_.logger->warn("stop() called before start() — no-op");
    return;
  }

  std::call_once(stopOnce_, [this] {
    // Phase 1: stop accepting new connections.
    std::error_code ec;
    acceptor_.close(ec);

    // Phase 2: snapshot sessions and initiate graceful close on each.
    // All close() calls are posted to ioc_ *before* the work guard is reset,
    // so ioc_.run() will not exit prematurely.
    asio::post(ioc_, [this] {
      std::vector<SessionPtr> snapshot;
      {
        std::lock_guard<std::mutex> lk(sessionsMutex_);
        if (sessions_.empty()) {
          workGuard_.reset();
          return;
        }

        // Arm the shutdown timer; cancelled early if all sessions drain first.
        aliveCnt_ = sessions_.size();
        shutdownTimer_.expires_after(
            std::chrono::seconds(config_.shutdownTimeoutSec));
        shutdownTimer_.async_wait([this](const std::error_code& ec) {
          if (ec) {
            return;
          }

          std::lock_guard<std::mutex> lk(sessionsMutex_);
          for (auto& [id, sess] : sessions_) {
            if (sess->isConnected()) {
              sess->forceClose({});
            }
          }
        });

        snapshot.reserve(sessions_.size());
        for (const auto& [_, sess] : sessions_) {
          snapshot.push_back(sess);
        }
      }

      for (auto& sess : snapshot) {
        sess->close();
      }

      workGuard_.reset();
    });
  });
}

// ── Accept loop ───────────────────────────────────────────────────────────
void TcpServer::doAccept()
{
  acceptor_.async_accept(
      asio::make_strand(ioc_),
      [this](std::error_code ec, asio::ip::tcp::socket socket) {
        if (ec) {
          // acceptor_.close() triggers this – stop the loop.
          if (ec != asio::error::operation_aborted) {
            config_.logger->error("accept error: {}", ec.message());
          }
          return;
        }

        asio::post(ioc_, [this, sock = std::move(socket)]() mutable {
          bool overloaded = false;
          {
            std::lock_guard<std::mutex> lk(sessionsMutex_);
            overloaded = (sessions_.size() >= config_.maxConnections);
          }

          if (overloaded) {
            std::error_code closeEc;
            sock.close(closeEc);
            if (overloadCb_)
              overloadCb_();
            config_.logger->warn(
                "max connections reached, rejected new connection");
            return;
          }

          auto sess = std::make_shared<TcpSession>(std::move(sock), config_);

          if (rawCb_) {
            sess->setRawCallback(rawCb_);
          }

          sess->setDisconnectCallback([this](SessionPtr s, std::error_code e) {
            if (disconnectCb_)
              disconnectCb_(s, e);
            asio::post(ioc_, [this, id = s->id()] { removeSession(id); });
          });

          if (errorCb_)
            sess->setErrorCallback(errorCb_);
          if (idleCb_)
            sess->setIdleCallback(idleCb_);

          {
            std::lock_guard<std::mutex> lk(sessionsMutex_);
            sessions_[sess->id()] = sess;
          }

          config_.logger->debug("new session {} from {}:{}",
                                sess->id(),
                                sess->remoteAddress(),
                                sess->remotePort());

          if (connectCb_)
            connectCb_(sess);
          sess->start();
        });

        doAccept();
      });
}

// ── Session helpers ───────────────────────────────────────────────────────
void TcpServer::addSession(SessionPtr sess)
{
  std::lock_guard<std::mutex> lk(sessionsMutex_);
  sessions_[sess->id()] = std::move(sess);
}

void TcpServer::removeSession(uint64_t id)
{
  std::lock_guard<std::mutex> lk(sessionsMutex_);
  sessions_.erase(id);

  if (aliveCnt_ && (*aliveCnt_)-- == 1) {
    std::error_code ec;
    shutdownTimer_.cancel(ec);
  }
}

SessionPtr TcpServer::getSession(uint64_t id)
{
  std::lock_guard<std::mutex> lk(sessionsMutex_);
  auto it = sessions_.find(id);
  if (it != sessions_.end())
    return it->second;
  return nullptr;
}

size_t TcpServer::sessionCount() const
{
  std::lock_guard<std::mutex> lk(sessionsMutex_);
  return sessions_.size();
}

} // namespace xas
