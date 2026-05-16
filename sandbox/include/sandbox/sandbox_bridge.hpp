#pragma once
// =============================================================================
// sandbox_bridge.hpp — Host ↔ Firecracker VM IPC Bridge
// =============================================================================
// The critical boundary between our trusted load generator and the untrusted
// contestant code running inside a Firecracker microVM.
//
// Data flow:
//   OrderBlaster → [BlastOrder ring] → SandboxBridge → [UDS] → VM
//   VM → [UDS] → SandboxBridge → [response ring] → Validator
//
// Timing boundary:
//   The official latency clock starts at asm_hot::rdtsc_serialized() the
//   INSTANT before we write to the Unix domain socket. It stops when we
//   read the contestant's response back from the socket.
//
// Backpressure:
//   If the contestant's socket buffer fills (they're too slow), we record
//   a DROP. Drops = penalty. We never block the host — non-blocking sends.
//
// Lock-free host side. Spin-based wait on response. Zero heap allocation.
// =============================================================================

#include "core/types.hpp"
#include "core/hot_path_asm.hpp"
#include "core/compiler_hints.hpp"
#include "core/spinlock.hpp"
#include "core/arena.hpp"
#include "sdk/protocol.hpp"
#include "loadgen/order_blaster.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

namespace iicpc {

// =============================================================================
// Response from contestant (what they send back after processing an order)
// =============================================================================
struct alignas(64) ContestantResponse {
    uint32_t order_id;         // Echo back the order's client_order_id
    MsgType  response_type;    // ORDER_ACK or FILL
    AckStatus ack_status;      // If ORDER_ACK
    uint8_t  _pad0;
    uint8_t  num_fills;        // Number of fills this order generated
    int64_t  fill_price;       // Fill price (if fill)
    int32_t  fill_qty;         // Fill quantity (if fill)
    int32_t  remaining_qty;    // Remaining on resting order
    uint64_t recv_tsc;         // TSC when we read this response (host-side)
    uint64_t send_tsc;         // TSC when we sent the order (host-side)
    uint8_t  _pad1[16];
};
static_assert(sizeof(ContestantResponse) == 64);

// =============================================================================
// Latency record (per-order timing)
// =============================================================================
struct OrderLatency {
    uint32_t order_id;
    uint64_t send_tsc;     // Host-side TSC at socket write
    uint64_t recv_tsc;     // Host-side TSC at socket read
    uint64_t latency_ns;   // Computed: (recv - send) / tsc_freq * 1e9
    bool     was_dropped;  // True if send() returned EAGAIN
};

// =============================================================================
// Bridge statistics
// =============================================================================
struct BridgeStats {
    uint64_t orders_sent      = 0;
    uint64_t responses_recv   = 0;
    uint64_t drops            = 0;   // Send failed (contestant too slow)
    uint64_t partial_sends    = 0;   // Partial write
    uint64_t recv_errors      = 0;
    uint64_t total_latency_ns = 0;   // Sum for average calculation
    uint64_t min_latency_ns   = UINT64_MAX;
    uint64_t max_latency_ns   = 0;
};

// =============================================================================
// Sandbox Bridge
// =============================================================================
inline constexpr uint32_t RESPONSE_RING_SIZE = 65536;
inline constexpr uint32_t LATENCY_RING_SIZE  = 65536;

class SandboxBridge {
public:
    SandboxBridge() noexcept = default;
    ~SandboxBridge() noexcept { shutdown(); }

    bool init(HugePageArena& arena, const char* socket_path) noexcept {
        socket_path_ = socket_path;

        // Allocate response ring
        responses_ = static_cast<ContestantResponse*>(
            arena.allocate_raw(sizeof(ContestantResponse) * RESPONSE_RING_SIZE,
                               CACHE_LINE_SIZE));
        latencies_ = static_cast<OrderLatency*>(
            arena.allocate_raw(sizeof(OrderLatency) * LATENCY_RING_SIZE,
                               CACHE_LINE_SIZE));

        if (!responses_ || !latencies_) return false;

        resp_head_ = 0;
        resp_tail_ = 0;
        lat_head_ = 0;
        lat_tail_ = 0;

        // Calibrate TSC frequency (10ms measurement window)
        calibrate_tsc_frequency();

        return true;
    }

    // =========================================================================
    // Create listening socket, wait for contestant VM to connect
    // =========================================================================
    [[nodiscard]] bool listen_and_accept(int timeout_ms = 10000) noexcept {
        ::unlink(socket_path_);

        listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return false;

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socket_path_, sizeof(addr.sun_path) - 1);

        if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr),
                   sizeof(addr)) < 0) {
            std::fprintf(stderr, "[bridge] bind(%s) failed: %s\n",
                         socket_path_, std::strerror(errno));
            return false;
        }

        ::listen(listen_fd_, 1);

        // Poll for connection
        struct pollfd pfd{};
        pfd.fd = listen_fd_;
        pfd.events = POLLIN;
        if (::poll(&pfd, 1, timeout_ms) <= 0) {
            std::fprintf(stderr, "[bridge] Timeout waiting for contestant\n");
            return false;
        }

        client_fd_ = ::accept(listen_fd_, nullptr, nullptr);
        if (client_fd_ < 0) return false;

        // Non-blocking + max socket buffer
        int flags = ::fcntl(client_fd_, F_GETFL, 0);
        ::fcntl(client_fd_, F_SETFL, flags | O_NONBLOCK);

        // Enlarge send/recv buffers (reduce drops)
        int bufsize = 1 << 20; // 1MB
        ::setsockopt(client_fd_, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
        ::setsockopt(client_fd_, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));

        std::fprintf(stderr, "[bridge] Contestant connected (fd=%d)\n",
                     client_fd_);
        return true;
    }

    // =========================================================================
    // Send an order to contestant (non-blocking, measures latency)
    // =========================================================================
    IICPC_HOT
    bool send_order(const BlastOrder& order) noexcept {
        if (client_fd_ < 0) return false;

        // === TIMING BOUNDARY: clock starts HERE ===
        uint64_t send_tsc = asm_hot::rdtsc_serialized();

        ssize_t n = ::send(client_fd_, order.data, order.size,
                           MSG_DONTWAIT | MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                stats_.drops++;
                // Record drop as latency entry
                record_latency(0, send_tsc, 0, true);
                return false;
            }
            stats_.recv_errors++;
            return false;
        }

        if (static_cast<uint32_t>(n) < order.size) {
            stats_.partial_sends++;
        }

        stats_.orders_sent++;

        // Store send_tsc for when we receive the response
        pending_tsc_[stats_.orders_sent % PENDING_TSC_SIZE] = send_tsc;
        pending_oid_[stats_.orders_sent % PENDING_TSC_SIZE] =
            (order.type == MsgType::ORDER_ENTRY)
                ? reinterpret_cast<const OrderEntry*>(order.data)->client_order_id
                : reinterpret_cast<const CancelRequest*>(order.data)->client_order_id;

        return true;
    }

    // =========================================================================
    // Receive responses from contestant (non-blocking, drain loop)
    // =========================================================================
    IICPC_HOT
    uint32_t recv_responses() noexcept {
        if (client_fd_ < 0) return 0;

        alignas(64) uint8_t buf[4096];
        uint32_t count = 0;

        while (true) {
            ssize_t n = ::recv(client_fd_, buf, sizeof(buf), MSG_DONTWAIT);
            if (n <= 0) break;

            // === TIMING BOUNDARY: clock stops HERE ===
            uint64_t recv_tsc = asm_hot::rdtscp_end();

            // Parse response(s) — contestant may batch
            std::size_t offset = 0;
            while (offset + sizeof(ContestantResponse) <= static_cast<std::size_t>(n)) {
                auto* resp = reinterpret_cast<ContestantResponse*>(buf + offset);
                resp->recv_tsc = recv_tsc;

                // Find matching send TSC
                for (uint32_t i = 0; i < PENDING_TSC_SIZE; ++i) {
                    if (pending_oid_[i] == resp->order_id) {
                        resp->send_tsc = pending_tsc_[i];
                        pending_oid_[i] = 0; // Consumed
                        break;
                    }
                }

                // Compute latency
                if (resp->send_tsc > 0) {
                    uint64_t cycles = recv_tsc - resp->send_tsc;
                    // Convert cycles to nanoseconds using calibrated TSC frequency
                    uint64_t ns = (tsc_ghz_ > 0.0)
                        ? static_cast<uint64_t>(static_cast<double>(cycles) / tsc_ghz_)
                        : cycles; // Fallback: assume ~1GHz
                    record_latency(resp->order_id, resp->send_tsc,
                                   recv_tsc, false);
                }

                // Store in response ring
                uint32_t ridx = resp_tail_ % RESPONSE_RING_SIZE;
                asm_hot::copy_cacheline(&responses_[ridx], resp);
                resp_tail_++;
                stats_.responses_recv++;
                count++;

                offset += sizeof(ContestantResponse);
            }
        }
        return count;
    }

    // =========================================================================
    // Pop a response for validation
    // =========================================================================
    [[nodiscard]] bool pop_response(ContestantResponse& out) noexcept {
        if (resp_head_ >= resp_tail_) return false;
        asm_hot::copy_cacheline(&out, &responses_[resp_head_ % RESPONSE_RING_SIZE]);
        resp_head_++;
        return true;
    }

    // =========================================================================
    // Pop a latency record
    // =========================================================================
    [[nodiscard]] bool pop_latency(OrderLatency& out) noexcept {
        if (lat_head_ >= lat_tail_) return false;
        std::memcpy(&out, &latencies_[lat_head_ % LATENCY_RING_SIZE], sizeof(out));
        lat_head_++;
        return true;
    }

    void shutdown() noexcept {
        if (client_fd_ >= 0) { ::close(client_fd_); client_fd_ = -1; }
        if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
        if (socket_path_) ::unlink(socket_path_);
    }

    [[nodiscard]] const BridgeStats& stats() const noexcept { return stats_; }
    [[nodiscard]] bool is_connected() const noexcept { return client_fd_ >= 0; }

    void print_stats() const noexcept {
        std::fprintf(stderr, "\n");
        std::fprintf(stderr, "╔══════════════════════════════════════════════════════════╗\n");
        std::fprintf(stderr, "║              SANDBOX BRIDGE STATISTICS                  ║\n");
        std::fprintf(stderr, "╠══════════════════════════════════════════════════════════╣\n");
        std::fprintf(stderr, "║  Orders Sent:     %-37lu ║\n", stats_.orders_sent);
        std::fprintf(stderr, "║  Responses Recv:  %-37lu ║\n", stats_.responses_recv);
        std::fprintf(stderr, "║  Drops (backpr.): %-37lu ║\n", stats_.drops);
        std::fprintf(stderr, "║  Partial Sends:   %-37lu ║\n", stats_.partial_sends);
        std::fprintf(stderr, "║  Recv Errors:     %-37lu ║\n", stats_.recv_errors);
        double drop_rate = (stats_.orders_sent > 0)
            ? 100.0 * static_cast<double>(stats_.drops) /
              static_cast<double>(stats_.orders_sent + stats_.drops)
            : 0.0;
        std::fprintf(stderr, "║  Drop Rate:       %-34.2f %% ║\n", drop_rate);
        std::fprintf(stderr, "╚══════════════════════════════════════════════════════════╝\n");
    }

private:
    void record_latency(uint32_t oid, uint64_t send, uint64_t recv,
                        bool dropped) noexcept {
        uint32_t idx = lat_tail_ % LATENCY_RING_SIZE;
        latencies_[idx].order_id = oid;
        latencies_[idx].send_tsc = send;
        latencies_[idx].recv_tsc = recv;
        latencies_[idx].latency_ns = (recv > send) ? (recv - send) : 0;
        latencies_[idx].was_dropped = dropped;
        lat_tail_++;
    }

    static constexpr uint32_t PENDING_TSC_SIZE = 4096;

    const char* socket_path_ = nullptr;
    int listen_fd_ = -1;
    int client_fd_ = -1;

    // Response ring
    ContestantResponse* responses_ = nullptr;
    uint64_t resp_head_ = 0;
    uint64_t resp_tail_ = 0;

    // Latency ring
    OrderLatency* latencies_ = nullptr;
    uint64_t lat_head_ = 0;
    uint64_t lat_tail_ = 0;

    // Pending send TSC tracking (circular buffer)
    uint64_t pending_tsc_[PENDING_TSC_SIZE] = {};
    uint32_t pending_oid_[PENDING_TSC_SIZE] = {};

    BridgeStats stats_{};
    double tsc_ghz_ = 0.0; // TSC frequency in GHz (cycles per nanosecond)

    void calibrate_tsc_frequency() noexcept {
        // Measure TSC ticks over a precise 10ms wall-clock interval
        constexpr int CALIBRATION_MS = 10;
        struct timespec ts_start, ts_end;
        uint64_t tsc_start = asm_hot::rdtsc_serialized();
        clock_gettime(CLOCK_MONOTONIC, &ts_start);

        // Busy-wait for calibration period
        struct timespec ts_now;
        do {
            clock_gettime(CLOCK_MONOTONIC, &ts_now);
        } while ((ts_now.tv_sec - ts_start.tv_sec) * 1000 +
                 (ts_now.tv_nsec - ts_start.tv_nsec) / 1000000 < CALIBRATION_MS);

        uint64_t tsc_end = asm_hot::rdtsc_serialized();
        clock_gettime(CLOCK_MONOTONIC, &ts_end);

        uint64_t tsc_delta = tsc_end - tsc_start;
        uint64_t ns_delta = (ts_end.tv_sec - ts_start.tv_sec) * 1000000000ULL +
                           (ts_end.tv_nsec - ts_start.tv_nsec);

        if (ns_delta > 0) {
            tsc_ghz_ = static_cast<double>(tsc_delta) / static_cast<double>(ns_delta);
            std::fprintf(stderr, "[bridge] TSC calibrated: %.3f GHz (%.0f cycles/%dms)\n",
                         tsc_ghz_, static_cast<double>(tsc_delta), CALIBRATION_MS);
        } else {
            tsc_ghz_ = 2.7; // Reasonable default
            std::fprintf(stderr, "[bridge] TSC calibration failed, using %.1f GHz default\n",
                         tsc_ghz_);
        }
    }
};

} // namespace iicpc
