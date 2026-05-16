#pragma once
// =============================================================================
// gateway_client.hpp — VM-Side Gateway Client (runs inside Firecracker)
// =============================================================================
// Lightweight Unix-domain-socket client. Connects to the host gateway,
// receives market data + execution reports, sends orders + cancels.
//
// This runs INSIDE the contestant VM. Linked into the strategy binary.
// Uses non-blocking poll loop — no threads, no heap, no bloat.
// =============================================================================

#include "sdk/protocol.hpp"
#include "sdk/strategy_sdk.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>

namespace iicpc {

// =============================================================================
// Position & PnL Tracker (runs inside VM, updated on fills)
// =============================================================================
struct PositionTracker {
    int32_t  position      = 0;
    int64_t  realized_pnl  = 0;
    int64_t  avg_entry_price = 0; // Volume-weighted average entry (scaled)
    int64_t  total_cost    = 0;   // Total cost basis
    int64_t  last_mid      = 0;   // Last mid price for mark-to-market
    uint32_t next_client_id = 1;

    void on_fill(const Fill& fill) noexcept {
        int32_t signed_qty = (fill.side == Side::BUY) ? fill.fill_qty : -fill.fill_qty;

        // Closing position
        if ((position > 0 && signed_qty < 0) || (position < 0 && signed_qty > 0)) {
            int32_t close_qty = (signed_qty > 0)
                ? ((signed_qty < -position) ? signed_qty : -position)
                : ((signed_qty > -position) ? signed_qty : -position);

            if (position > 0) { // Was long, selling
                realized_pnl += static_cast<int64_t>(close_qty) *
                    (avg_entry_price - fill.fill_price) * -1;
            } else { // Was short, buying
                realized_pnl += static_cast<int64_t>(-close_qty) *
                    (fill.fill_price - avg_entry_price) * -1;
            }
        }

        // Opening/extending position
        if ((position >= 0 && signed_qty > 0) || (position <= 0 && signed_qty < 0)) {
            int64_t old_cost = avg_entry_price * static_cast<int64_t>(
                (position >= 0) ? position : -position);
            int64_t new_cost = fill.fill_price * static_cast<int64_t>(
                (signed_qty >= 0) ? signed_qty : -signed_qty);
            int32_t new_total = (position >= 0 ? position : -position) +
                                (signed_qty >= 0 ? signed_qty : -signed_qty);
            if (new_total > 0) {
                avg_entry_price = (old_cost + new_cost) / new_total;
            }
        }

        position += signed_qty;
    }

    [[nodiscard]] int64_t unrealized_pnl() const noexcept {
        if (position == 0 || last_mid == 0) return 0;
        if (position > 0) {
            return static_cast<int64_t>(position) * (last_mid - avg_entry_price);
        } else {
            return static_cast<int64_t>(-position) * (avg_entry_price - last_mid);
        }
    }

    [[nodiscard]] int64_t total_pnl() const noexcept {
        return realized_pnl + unrealized_pnl();
    }
};

// =============================================================================
// Gateway Client — IOrderGateway implementation over Unix socket
// =============================================================================
class GatewayClient final : public IOrderGateway {
public:
    GatewayClient() noexcept = default;
    ~GatewayClient() noexcept { disconnect(); }

    [[nodiscard]] bool connect(const char* socket_path) noexcept {
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) {
            std::fprintf(stderr, "[gateway_client] socket() failed: %s\n",
                         std::strerror(errno));
            return false;
        }

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

        if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr),
                      sizeof(addr)) < 0) {
            std::fprintf(stderr, "[gateway_client] connect(%s) failed: %s\n",
                         socket_path, std::strerror(errno));
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        return true;
    }

    void disconnect() noexcept {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    // =========================================================================
    // Main event loop — drives the strategy
    // =========================================================================
    void run(IStrategy& strategy) noexcept {
        if (fd_ < 0) return;

        // Aligned receive buffer (largest message = 64 bytes)
        alignas(64) uint8_t buf[256];

        struct pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;

        bool session_active = false;

        while (true) {
            int ret = ::poll(&pfd, 1, 1000); // 1s timeout for heartbeat
            if (ret < 0) {
                if (errno == EINTR) continue;
                break;
            }

            if (ret == 0) {
                // Timeout — send heartbeat
                Heartbeat hb{};
                hb.msg_type = MsgType::HEARTBEAT;
                hb.sequence = tracker_.next_client_id;
                send_raw(&hb, sizeof(hb));
                continue;
            }

            // Read message
            ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
            if (n <= 0) break; // Disconnected

            // Dispatch based on message type
            MsgType type = peek_msg_type(buf);

            switch (type) {
                case MsgType::SESSION_START: {
                    auto& msg = *reinterpret_cast<SessionStart*>(buf);
                    strategy.on_session_start(*this, msg);
                    session_active = true;
                    break;
                }
                case MsgType::MARKET_UPDATE: {
                    auto& msg = *reinterpret_cast<MarketUpdate*>(buf);
                    tracker_.last_mid = (msg.best_bid_price + msg.best_ask_price) / 2;
                    strategy.on_market_data(msg);
                    break;
                }
                case MsgType::ORDER_ACK: {
                    auto& msg = *reinterpret_cast<OrderAck*>(buf);
                    strategy.on_order_ack(msg);
                    break;
                }
                case MsgType::FILL: {
                    auto& msg = *reinterpret_cast<Fill*>(buf);
                    tracker_.on_fill(msg);
                    strategy.on_fill(msg);
                    break;
                }
                case MsgType::CANCEL_ACK: {
                    auto& msg = *reinterpret_cast<CancelAck*>(buf);
                    strategy.on_cancel_ack(msg);
                    break;
                }
                case MsgType::SESSION_END: {
                    auto& msg = *reinterpret_cast<SessionEnd*>(buf);
                    strategy.on_session_end(msg);
                    session_active = false;
                    return; // Match is over
                }
                case MsgType::HEARTBEAT:
                    break; // Just keep alive
                default:
                    break;
            }
        }

        // If we reach here without SESSION_END, something went wrong
        if (session_active) {
            SessionEnd end{};
            end.msg_type = MsgType::SESSION_END;
            end.final_pnl = tracker_.total_pnl();
            end.final_position = tracker_.position;
            strategy.on_session_end(end);
        }
    }

    // =========================================================================
    // IOrderGateway implementation
    // =========================================================================
    [[nodiscard]] bool send_order(const OrderEntry& order) noexcept override {
        return send_raw(&order, sizeof(order));
    }

    [[nodiscard]] bool send_cancel(const CancelRequest& cancel) noexcept override {
        return send_raw(&cancel, sizeof(cancel));
    }

    [[nodiscard]] int32_t position() const noexcept override {
        return tracker_.position;
    }

    [[nodiscard]] int64_t unrealized_pnl() const noexcept override {
        return tracker_.unrealized_pnl();
    }

    [[nodiscard]] int64_t realized_pnl() const noexcept override {
        return tracker_.realized_pnl;
    }

    [[nodiscard]] int64_t total_pnl() const noexcept override {
        return tracker_.total_pnl();
    }

    [[nodiscard]] uint32_t next_order_id() noexcept override {
        return tracker_.next_client_id++;
    }

private:
    bool send_raw(const void* data, std::size_t len) noexcept {
        if (fd_ < 0) return false;
        ssize_t sent = ::send(fd_, data, len, MSG_NOSIGNAL);
        return sent == static_cast<ssize_t>(len);
    }

    int fd_ = -1;
    PositionTracker tracker_;
};

} // namespace iicpc
