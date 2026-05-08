#include "xas/TcpSession.h"

#include <system_error>

namespace xas {

// ── Static member definition ──────────────────────────────────────────────
std::atomic<uint64_t> TcpSession::nextId_{1};

// ── Constructor ───────────────────────────────────────────────────────────
TcpSession::TcpSession(asio::ip::tcp::socket socket,
                       const ServerConfig& config)
    : id_(nextId_++)
    , socket_(std::move(socket))
    , strand_(asio::make_strand(socket_.get_executor()))
    , idleTimer_(socket_.get_executor())
    , config_(config)
{
    std::error_code ec;

    if (config_.tcpNoDelay) {
        socket_.set_option(asio::ip::tcp::no_delay(true), ec);
        // ignore option errors (best-effort)
    }

    if (config_.keepAlive) {
        socket_.set_option(asio::socket_base::keep_alive(true), ec);
    }
}

// ── Public API ────────────────────────────────────────────────────────────
std::string TcpSession::remoteAddress() const
{
    try {
        return socket_.remote_endpoint().address().to_string();
    } catch (...) {
        return {};
    }
}

uint16_t TcpSession::remotePort() const
{
    try {
        return socket_.remote_endpoint().port();
    } catch (...) {
        return 0;
    }
}

void TcpSession::send(const Buffer& data)
{
    Buffer copy = data;
    asio::post(strand_, [self = shared_from_this(), buf = std::move(copy)]() mutable {
        self->writeQueue_.push_back(std::move(buf));
        if (!self->writing_) {
            self->doWrite();
        }
    });
}

void TcpSession::send(Buffer&& data)
{
    asio::post(strand_, [self = shared_from_this(), buf = std::move(data)]() mutable {
        self->writeQueue_.push_back(std::move(buf));
        if (!self->writing_) {
            self->doWrite();
        }
    });
}

void TcpSession::close()
{
    asio::post(strand_, [self = shared_from_this()]() {
        if (!self->connected_) return;

        if (!self->writeQueue_.empty() || self->writing_) {
            // Drain the write queue first; doWrite will check closing_ when done.
            self->closing_ = true;
        } else {
            // Nothing pending – shut down immediately.
            std::error_code ec;
            self->socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            self->socket_.close(ec);
            // disconnectCb_ will be called via forceClose path if needed,
            // but a clean user-initiated close is treated as a clean disconnect.
            self->connected_ = false;
            self->idleTimer_.cancel();
            if (self->disconnectCb_) {
                self->disconnectCb_(self->shared_from_this(), std::error_code{});
            }
        }
    });
}

// ── Callback setters ──────────────────────────────────────────────────────
void TcpSession::setRawCallback(std::function<void(SessionPtr, Buffer&)> cb)
{
    rawCb_ = std::move(cb);
}

void TcpSession::setDisconnectCallback(std::function<void(SessionPtr, std::error_code)> cb)
{
    disconnectCb_ = std::move(cb);
}

void TcpSession::setErrorCallback(std::function<void(SessionPtr, std::error_code)> cb)
{
    errorCb_ = std::move(cb);
}

void TcpSession::setIdleCallback(std::function<void(SessionPtr)> cb)
{
    idleCb_ = std::move(cb);
}

// ── start() ───────────────────────────────────────────────────────────────
void TcpSession::start()
{
    asio::post(strand_, [self = shared_from_this()]() {
        self->doRead();
        self->resetIdleTimer();
    });
}

// ── Private implementation ────────────────────────────────────────────────
void TcpSession::doRead()
{
    auto self = shared_from_this();
    asio::async_read(
        socket_,
        asio::dynamic_buffer(receiveBuffer_),
        asio::transfer_at_least(1),
        asio::bind_executor(strand_,
            [self](const std::error_code& ec, std::size_t /*bytesTransferred*/) {
                if (ec) {
                    self->forceClose(ec);
                    return;
                }

                if (self->receiveBuffer_.size() > self->config_.maxReceiveBuffer) {
                    if (self->errorCb_) {
                        self->errorCb_(self->shared_from_this(),
                                       std::make_error_code(std::errc::value_too_large));
                    }
                    self->forceClose(std::make_error_code(std::errc::value_too_large));
                    return;
                }

                if (self->rawCb_) {
                    self->rawCb_(self->shared_from_this(), self->receiveBuffer_);
                }

                self->resetIdleTimer();
                self->doRead();
            }));
}

void TcpSession::doWrite()
{
    if (writeQueue_.empty()) {
        writing_ = false;

        if (closing_) {
            // All data has been flushed; now perform the clean shutdown.
            std::error_code ec;
            socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            socket_.close(ec);
            connected_ = false;
            idleTimer_.cancel();
            if (disconnectCb_) {
                disconnectCb_(shared_from_this(), std::error_code{});
            }
        }
        return;
    }

    writing_ = true;
    auto self = shared_from_this();
    asio::async_write(
        socket_,
        asio::buffer(writeQueue_.front()),
        asio::bind_executor(strand_,
            [self](const std::error_code& ec, std::size_t /*bytesTransferred*/) {
                if (ec) {
                    self->forceClose(ec);
                    return;
                }

                self->writeQueue_.pop_front();
                self->doWrite();   // handles empty-queue + closing_ check
            }));
}

void TcpSession::resetIdleTimer()
{
    if (config_.idleTimeoutSec == 0) return;

    idleTimer_.expires_after(std::chrono::seconds(config_.idleTimeoutSec));
    idleTimer_.async_wait(
        asio::bind_executor(strand_,
            [self = shared_from_this()](const std::error_code& ec) {
                self->onIdleTimeout(ec);
            }));
}

void TcpSession::onIdleTimeout(const std::error_code& ec)
{
    if (ec == asio::error::operation_aborted) return;

    if (idleCb_) {
        idleCb_(shared_from_this());
    }
    // Intentionally NOT auto-closing: user callback decides.
}

void TcpSession::forceClose(std::error_code reason)
{
    if (!connected_) return;

    connected_ = false;
    idleTimer_.cancel();

    std::error_code ec;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec);

    if (disconnectCb_) {
        disconnectCb_(shared_from_this(), reason);
    }
}

} // namespace xas
