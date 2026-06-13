#pragma once

// --- Gateway Client (Contestant Engine Runner) ---
// Connects to the host platform via Unix domain socket.
// Receives OrderEntry/CancelRequest from the order blaster.
// Dispatches to contestant's IStrategy implementation.
// Batched response sends with MSG_NOSIGNAL.

#include "sdk/protocol.hpp"
#include "sdk/strategy_sdk.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace iicpc {

// --- Response Sender — batches responses for throughput ---
class SocketResponseSender final : public IResponseSender {
public:
    explicit SocketResponseSender(int fd) noexcept : fd_(fd) {}

    bool send(const ContestantResponse& resp) noexcept override {
        batch_[count_++] = resp;
        if (count_ >= BATCH_SIZE) {
            return flush();
        }
        return true;
    }

    bool flush() noexcept {
        if (count_ == 0) return true;
        ssize_t total = static_cast<ssize_t>(sizeof(ContestantResponse) * count_);
        ssize_t sent = ::send(fd_, batch_, total, MSG_NOSIGNAL);
        count_ = 0;
        return sent == total;
    }

private:
    static constexpr uint32_t BATCH_SIZE = 64;
    int fd_;
    ContestantResponse batch_[BATCH_SIZE] = {};
    uint32_t count_ = 0;
};

// --- Gateway Client — connects to platform, drives contestant's engine ---
class GatewayClient {
public:
    GatewayClient() noexcept = default;
    ~GatewayClient() noexcept { disconnect(); }

    [[nodiscard]] bool connect(const char* socket_path) noexcept {
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) {
            std::fprintf(stderr, "[engine] socket() failed: %s\n",
                         std::strerror(errno));
            return false;
        }

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

        if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr),
                      sizeof(addr)) < 0) {
            std::fprintf(stderr, "[engine] connect(%s) failed: %s\n",
                         socket_path, std::strerror(errno));
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        // Enlarge socket buffers for high-throughput reads
        int bufsize = 1 << 20; // 1MB
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));

        return true;
    }

    void disconnect() noexcept {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    // --- Main event loop — drives the contestant's engine ---
    void run(IStrategy& strategy) noexcept {
        if (fd_ < 0) return;

        SocketResponseSender sender(fd_);

        static constexpr size_t BUF_SIZE = 65536;
        alignas(64) uint8_t buf[BUF_SIZE];
        size_t buf_used = 0;

        uint64_t orders_processed = 0;
        uint64_t cancels_processed = 0;

        while (true) {
            ssize_t n = ::recv(fd_, buf + buf_used, BUF_SIZE - buf_used, 0);
            if (n <= 0) break;
            buf_used += static_cast<size_t>(n);

            // Process all complete messages in the buffer
            size_t offset = 0;
            while (offset < buf_used) {
                if (offset >= buf_used) break;

                MsgType type = peek_msg_type(buf + offset);
                uint32_t msize = msg_size(type);

                if (msize == 0 || offset + msize > buf_used) break;

                switch (type) {
                    case MsgType::SESSION_START: {
                        auto& msg = *reinterpret_cast<SessionStart*>(buf + offset);
                        strategy.on_session_start(msg);
                        std::fprintf(stderr, "[engine] Session started: "
                                     "duration=%ums instruments=%u\n",
                                     msg.match_duration_ms, msg.instrument_count);
                        break;
                    }

                    case MsgType::ORDER_ENTRY: {
                        auto& msg = *reinterpret_cast<OrderEntry*>(buf + offset);
                        strategy.on_order(msg, sender);
                        orders_processed++;
                        break;
                    }

                    case MsgType::CANCEL_REQUEST: {
                        auto& msg = *reinterpret_cast<CancelRequest*>(buf + offset);
                        strategy.on_cancel(msg, sender);
                        cancels_processed++;
                        break;
                    }

                    case MsgType::SESSION_END: {
                        sender.flush();
                        auto& msg = *reinterpret_cast<SessionEnd*>(buf + offset);
                        strategy.on_session_end(msg);
                        std::fprintf(stderr, "[engine] Session ended: "
                                     "orders=%lu cancels=%lu\n",
                                     orders_processed, cancels_processed);
                        return;
                    }

                    case MsgType::HEARTBEAT:
                        break;

                    default:
                        std::fprintf(stderr, "[engine] Unknown msg type: %u\n",
                                     static_cast<unsigned>(type));
                        break;
                }

                offset += msize;
            }

            // Flush pending responses after processing batch
            sender.flush();

            // Move unprocessed bytes to start of buffer
            if (offset > 0 && offset < buf_used) {
                std::memmove(buf, buf + offset, buf_used - offset);
                buf_used -= offset;
            } else if (offset >= buf_used) {
                buf_used = 0;
            }
        }

        // Connection closed without SESSION_END
        sender.flush();
        std::fprintf(stderr, "[engine] Connection closed: "
                     "orders=%lu cancels=%lu\n",
                     orders_processed, cancels_processed);
    }

private:
    int fd_ = -1;
};

} // namespace iicpc
