#pragma once
// =============================================================================
// gateway.hpp — Host-Side Gateway (Exchange ↔ Contestant VM Bridge)
// =============================================================================
// Per-contestant gateway running on the host. Bridges:
//   - Exchange side: direct function calls to MatchEngine (same process)
//   - Contestant side: Unix domain socket (reaches into Firecracker VM)
//
// Responsibilities:
//   - Forward market data ticks to contestant
//   - Receive orders from contestant, validate, forward to engine
//   - Forward fills/acks back to contestant
//   - Rate limiting, heartbeat monitoring
//   - PnL tracking (authoritative, not contestant's self-report)
// =============================================================================

#include "exchange/match_engine.hpp"
#include "sdk/protocol.hpp"
#include "core/compiler_hints.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

namespace iicpc {

struct GatewayConfig {
    const char* socket_path  = "/tmp/iicpc_gateway.sock";
    uint32_t    contestant_id = 0;
    int32_t     max_orders_per_sec = 1000;
    int32_t     heartbeat_timeout_ms = 5000;
};

class Gateway {
public:
    Gateway() noexcept = default;
    ~Gateway() noexcept { shutdown(); }

    bool init(const GatewayConfig& cfg) noexcept {
        cfg_ = cfg;
        return true;
    }

    // =========================================================================
    // Create listening socket and wait for contestant to connect
    // =========================================================================
    [[nodiscard]] bool listen_and_accept() noexcept {
        ::unlink(cfg_.socket_path);

        listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            std::fprintf(stderr, "[gateway] socket() failed: %s\n",
                         std::strerror(errno));
            return false;
        }

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, cfg_.socket_path,
                     sizeof(addr.sun_path) - 1);

        if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr),
                   sizeof(addr)) < 0) {
            std::fprintf(stderr, "[gateway] bind(%s) failed: %s\n",
                         cfg_.socket_path, std::strerror(errno));
            return false;
        }

        if (::listen(listen_fd_, 1) < 0) {
            std::fprintf(stderr, "[gateway] listen() failed: %s\n",
                         std::strerror(errno));
            return false;
        }

        std::fprintf(stderr, "[gateway] Listening on %s\n", cfg_.socket_path);

        // Wait for contestant to connect (with timeout)
        struct pollfd pfd{};
        pfd.fd = listen_fd_;
        pfd.events = POLLIN;
        int ret = ::poll(&pfd, 1, cfg_.heartbeat_timeout_ms);
        if (ret <= 0) {
            std::fprintf(stderr, "[gateway] Timeout waiting for contestant\n");
            return false;
        }

        client_fd_ = ::accept(listen_fd_, nullptr, nullptr);
        if (client_fd_ < 0) {
            std::fprintf(stderr, "[gateway] accept() failed: %s\n",
                         std::strerror(errno));
            return false;
        }

        // Set non-blocking for the client
        int flags = ::fcntl(client_fd_, F_GETFL, 0);
        ::fcntl(client_fd_, F_SETFL, flags | O_NONBLOCK);

        // TCP_NODELAY equivalent for UDS — disable Nagle
        int one = 1;
        ::setsockopt(client_fd_, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

        std::fprintf(stderr, "[gateway] Contestant connected (fd=%d)\n",
                     client_fd_);
        return true;
    }

    // =========================================================================
    // Send a message to the contestant
    // =========================================================================
    bool send_to_contestant(const void* data, uint32_t size) noexcept {
        if (client_fd_ < 0) return false;
        ssize_t sent = ::send(client_fd_, data, size, MSG_NOSIGNAL);
        return sent == static_cast<ssize_t>(size);
    }

    // =========================================================================
    // Receive a message from contestant (non-blocking)
    // Returns message type, or 0 if nothing available
    // =========================================================================
    IICPC_HOT
    MsgType recv_from_contestant(void* buf, uint32_t buf_size) noexcept {
        if (client_fd_ < 0) return static_cast<MsgType>(0);

        ssize_t n = ::recv(client_fd_, buf, buf_size, MSG_DONTWAIT);
        if (n <= 0) return static_cast<MsgType>(0);

        return peek_msg_type(buf);
    }

    // =========================================================================
    // Send session start to contestant
    // =========================================================================
    void send_session_start(const MatchEngineConfig& cfg,
                            uint64_t start_ts) noexcept {
        SessionStart ss{};
        ss.msg_type = MsgType::SESSION_START;
        ss.instrument_count = 1;
        ss.match_duration_ms = cfg.match_duration_ms;
        ss.start_timestamp_ns = start_ts;
        ss.initial_cash = cfg.initial_cash;
        ss.max_position = cfg.max_position;
        ss.max_order_size = cfg.max_order_size;
        ss.max_orders_per_sec = cfg.max_orders_per_sec;
        send_to_contestant(&ss, sizeof(ss));
    }

    // =========================================================================
    // Send market data to contestant
    // =========================================================================
    IICPC_HOT
    void send_market_data(const MarketUpdate& update) noexcept {
        send_to_contestant(&update, sizeof(update));
    }

    // =========================================================================
    // Forward outbound messages (fills/acks) to contestant
    // =========================================================================
    void forward_outbound(const OutboundMsg& msg) noexcept {
        if (msg.contestant_id == cfg_.contestant_id) {
            send_to_contestant(msg.data, msg.size);
        }
    }

    // =========================================================================
    // Send session end
    // =========================================================================
    void send_session_end(const SessionEnd& end) noexcept {
        send_to_contestant(&end, sizeof(end));
    }

    void shutdown() noexcept {
        if (client_fd_ >= 0) { ::close(client_fd_); client_fd_ = -1; }
        if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
        if (cfg_.socket_path) ::unlink(cfg_.socket_path);
    }

    [[nodiscard]] int client_fd() const noexcept { return client_fd_; }
    [[nodiscard]] bool is_connected() const noexcept { return client_fd_ >= 0; }

private:
    GatewayConfig cfg_{};
    int listen_fd_ = -1;
    int client_fd_ = -1;
};

} // namespace iicpc
