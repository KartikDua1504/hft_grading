#pragma once
// =============================================================================
// pipeline_engine.hpp — Multiplexed Pipeline Engine (writev + readv)
// =============================================================================
// Instead of 1 socket per bot (N send() syscalls per batch), use a small pool
// of connections and pipeline multiple bot messages through each socket.
//
// Architecture:
//   - Pool of K sockets (K << N bots), default K=4
//   - Round-robin bot assignment to sockets
//   - writev() to batch multiple messages into ONE syscall
//   - readv() to receive multiple ACKs in ONE syscall
//   - This reduces syscall count from N to 2*K per batch
//
// Expected improvement: from ~100µs to <20µs by eliminating syscall overhead
// =============================================================================

#include "core/types.hpp"
#include "core/tsc.hpp"
#include "core/ring_buffer.hpp"
#include "core/compiler_hints.hpp"
#include "loadgen/bot_fleet.hpp"
#include "loadgen/payload_gen.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

namespace iicpc {

// =============================================================================
// Compile-time configuration
// =============================================================================
template<std::size_t PoolSize = 4, std::size_t MaxBatch = 512>
struct PipelineConfig {
    static constexpr std::size_t POOL_SIZE = PoolSize;
    static constexpr std::size_t MAX_BATCH = MaxBatch;
    static constexpr std::size_t MAX_IOV = 1024;  // max iovecs per writev

    const char* target_host = "127.0.0.1";
    uint16_t target_port = 9999;
    const char* unix_socket_path = nullptr;
    int cpu_affinity = -1;
};

// =============================================================================
// Pipeline Engine — CRTP, multiplexed connections
// =============================================================================
template<std::size_t PoolSize = 4, std::size_t MaxBatch = 512>
class PipelineEngine {
public:
    using TelemetryRing = SPSCRingBuffer<LatencySample, 1048576>;
    static constexpr std::size_t POOL = PoolSize;

    PipelineEngine() noexcept = default;
    ~PipelineEngine() noexcept { shutdown(); }

    [[nodiscard]] bool init(const PipelineConfig<PoolSize, MaxBatch>& config,
                            BotFleet& fleet,
                            HugePageArena& arena) noexcept {
        config_ = config;

        // Pin CPU
        if (config.cpu_affinity >= 0) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(config.cpu_affinity, &cpuset);
            ::sched_setaffinity(0, sizeof(cpuset), &cpuset);
        }

        // Allocate iovec arrays from arena
        for (std::size_t p = 0; p < POOL; ++p) {
            send_iovs_[p] = static_cast<struct iovec*>(
                arena.allocate_raw(sizeof(struct iovec) * MaxBatch));
            if (!send_iovs_[p]) return false;
        }

        recv_buf_ = arena.allocate<uint8_t>(65536);  // 64K recv buffer
        if (!recv_buf_) return false;

        // Bot-to-message index mapping
        batch_bot_indices_ = static_cast<std::size_t*>(
            arena.allocate_raw(sizeof(std::size_t) * MaxBatch));
        batch_pipe_indices_ = static_cast<std::size_t*>(
            arena.allocate_raw(sizeof(std::size_t) * MaxBatch));

        // Connect socket pool
        std::size_t connected = 0;
        for (std::size_t p = 0; p < POOL; ++p) {
            int fd;
            if (config.unix_socket_path) {
                fd = connect_unix(config.unix_socket_path);
            } else {
                fd = connect_tcp(config.target_host, config.target_port);
            }

            if (fd < 0) {
                pool_fds_[p] = -1;
                continue;
            }

            // Set non-blocking
            int flags = ::fcntl(fd, F_GETFL, 0);
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

            // Maximize buffers
            int bufsize = 1 << 21; // 2 MiB
            ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
            ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));

            pool_fds_[p] = fd;
            connected++;
        }

        // Assign bots to sockets round-robin
        for (std::size_t i = 0; i < fleet.count; ++i) {
            fleet.socket_fds[i] = static_cast<int32_t>(i % POOL); // store pipe index, not fd
            fleet.states[i] = BotState::IDLE;
        }

        std::fprintf(stderr, "[pipeline] %zu/%zu pool connections (%s), %zu bots assigned\n",
                     connected, POOL,
                     config.unix_socket_path ? "Unix" : "TCP",
                     fleet.count);
        return connected > 0;
    }

    IICPC_HOT IICPC_FLATTEN
    std::size_t run_batch(BotFleet& fleet, TelemetryRing& ring) noexcept {
        // Phase 1: Gather IDLE bots, group by pipe, writev
        send_pipelined(fleet);

        // Phase 2: Read ACKs from all pipes, push to telemetry
        return recv_pipelined(fleet, ring);
    }

    void shutdown() noexcept {
        for (std::size_t p = 0; p < POOL; ++p) {
            if (pool_fds_[p] >= 0) {
                ::close(pool_fds_[p]);
                pool_fds_[p] = -1;
            }
        }
    }

    [[nodiscard]] uint64_t total_sends() const noexcept { return total_sends_; }
    [[nodiscard]] uint64_t total_recvs() const noexcept { return total_recvs_; }

private:
    // =========================================================================
    // Pipelined send: gather all IDLE bots, writev per pipe
    // =========================================================================
    IICPC_HOT
    void send_pipelined(BotFleet& fleet) noexcept {
        // Count messages per pipe
        std::size_t pipe_count[POOL] = {};
        std::size_t pipe_iov_idx[POOL] = {};
        std::size_t total_batch = 0;

        for (std::size_t i = 0; i < fleet.count && total_batch < MaxBatch; ++i) {
            // Prefetch
            if (IICPC_LIKELY(i + 8 < fleet.count)) {
                prefetch_read_l1(&fleet.states[i + 8]);
            }

            if (IICPC_UNLIKELY(fleet.states[i] != BotState::IDLE)) continue;

            const std::size_t pipe = static_cast<std::size_t>(fleet.socket_fds[i]);
            if (IICPC_UNLIKELY(pipe >= POOL || pool_fds_[pipe] < 0)) continue;

            // Patch payload
            const uint64_t tsc_now = rdtsc();
            const uint32_t seq = next_seq_++;
            patch_payload(fleet.payloads[i], seq, tsc_now);

            fleet.send_tsc[i] = tsc_now;
            fleet.sequence_nums[i] = seq;

            // Add to pipe's iovec
            const std::size_t iov_idx = pipe_count[pipe];
            send_iovs_[pipe][iov_idx].iov_base = const_cast<uint8_t*>(fleet.payloads[i]);
            send_iovs_[pipe][iov_idx].iov_len = fleet.payload_lens[i];
            pipe_count[pipe]++;

            fleet.states[i] = BotState::WAITING;
            batch_bot_indices_[total_batch] = i;
            batch_pipe_indices_[total_batch] = pipe;
            total_batch++;
        }

        if (IICPC_UNLIKELY(total_batch == 0)) return;

        // writev per pipe — ONE syscall per pipe instead of N per bot
        for (std::size_t p = 0; p < POOL; ++p) {
            if (pipe_count[p] == 0 || pool_fds_[p] < 0) continue;

            const ssize_t written = ::writev(pool_fds_[p],
                                              send_iovs_[p],
                                              static_cast<int>(pipe_count[p]));

            if (IICPC_LIKELY(written > 0)) {
                total_sends_ += pipe_count[p];
            }
        }
    }

    // =========================================================================
    // Pipelined recv: read all available ACKs from all pipes
    // =========================================================================
    IICPC_HOT
    std::size_t recv_pipelined(BotFleet& fleet, TelemetryRing& ring) noexcept {
        std::size_t total_recvd = 0;

        for (std::size_t p = 0; p < POOL; ++p) {
            if (pool_fds_[p] < 0) continue;

            while (true) {
                const ssize_t n = ::recv(pool_fds_[p], recv_buf_, 65536, MSG_DONTWAIT);
                if (IICPC_UNLIKELY(n <= 0)) break;

                // Parse ACK messages from buffer
                std::size_t offset = 0;
                while (offset + sizeof(AckMessage) <= static_cast<std::size_t>(n)) {
                    const auto* ack = reinterpret_cast<const AckMessage*>(recv_buf_ + offset);

                    if (IICPC_UNLIKELY(ack->magic != 0x41434B00)) {
                        offset++;
                        continue;
                    }

                    const uint64_t recv_tsc_now = rdtsc();

                    // Find the bot by ID and update state
                    const std::size_t bot_idx = ack->bot_id;
                    if (IICPC_LIKELY(bot_idx < fleet.count)) {
                        fleet.recv_tsc[bot_idx] = recv_tsc_now;
                        fleet.states[bot_idx] = BotState::IDLE;

                        const LatencySample sample{
                            .send_tsc = fleet.send_tsc[bot_idx],
                            .recv_tsc = recv_tsc_now,
                            .bot_id   = fleet.bot_ids[bot_idx],
                            .seq_num  = ack->seq_num,
                        };
                        (void)ring.try_push(sample);
                    }

                    total_recvs_++;
                    total_recvd++;
                    offset += sizeof(AckMessage);
                }
            }
        }

        return total_recvd;
    }

    // =========================================================================
    // Socket helpers
    // =========================================================================
    static int connect_tcp(const char* host, uint16_t port) noexcept {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        int flag = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        ::setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(flag));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        ::inet_pton(AF_INET, host, &addr.sin_addr);

        if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd);
            return -1;
        }
        return fd;
    }

    static int connect_unix(const char* path) noexcept {
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

        if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd);
            return -1;
        }
        return fd;
    }

    // =========================================================================
    // Data members
    // =========================================================================
    PipelineConfig<PoolSize, MaxBatch> config_{};
    int pool_fds_[POOL] = {};
    struct iovec* send_iovs_[POOL] = {};
    uint8_t* recv_buf_ = nullptr;
    std::size_t* batch_bot_indices_ = nullptr;
    std::size_t* batch_pipe_indices_ = nullptr;

    uint64_t total_sends_ = 0;
    uint64_t total_recvs_ = 0;
    uint32_t next_seq_ = 0;
};

} // namespace iicpc
