// =============================================================================
// io_engine.cpp — Epoll I/O Engine Implementation
// =============================================================================
// This is the fallback engine. io_uring engine will be added when liburing
// is available. The epoll engine is fully functional for Stage 1 validation.
// =============================================================================

#include "loadgen/io_engine.hpp"
#include "core/tsc.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace iicpc {

// =============================================================================
// Helper: set socket to non-blocking
// =============================================================================
static bool set_nonblocking(int fd) noexcept {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// =============================================================================
// Helper: create and connect a TCP socket
// =============================================================================
static int create_connected_socket(const char* host, uint16_t port) noexcept {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    // TCP_NODELAY — disable Nagle's algorithm for latency
    int flag = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, host, &addr.sin_addr);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }

    set_nonblocking(fd);
    return fd;
}

// =============================================================================
// EpollIoEngine Implementation
// =============================================================================

EpollIoEngine::~EpollIoEngine() noexcept {
    shutdown();
}

bool EpollIoEngine::init(const IoEngineConfig& config,
                          BotFleet& fleet,
                          HugePageArena& arena) noexcept {
    config_ = config;

    // Pin to CPU if requested
    if (config.cpu_affinity >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(config.cpu_affinity, &cpuset);
        if (::sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
            std::fprintf(stderr, "[io_engine] WARNING: Failed to pin to CPU %d: %s\n",
                         config.cpu_affinity, std::strerror(errno));
        }
    }

    // Create epoll instance
    epoll_fd_ = ::epoll_create1(0);
    if (epoll_fd_ < 0) {
        std::fprintf(stderr, "[io_engine] Failed to create epoll: %s\n", std::strerror(errno));
        return false;
    }

    // Allocate receive buffer from arena
    recv_buf_ = arena.allocate<uint8_t>(sizeof(AckMessage) * config.batch_size);
    if (!recv_buf_) return false;

    // Connect all bots to the exchange
    std::size_t connected = 0;
    for (std::size_t i = 0; i < fleet.count; ++i) {
        int fd = create_connected_socket(config.target_host, config.target_port);
        if (fd < 0) {
            // Only warn — some bots might fail to connect
            if (i < 5) {
                std::fprintf(stderr, "[io_engine] Bot %zu failed to connect: %s\n",
                             i, std::strerror(errno));
            }
            fleet.socket_fds[i] = -1;
            continue;
        }

        fleet.socket_fds[i] = fd;

        struct epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
        ev.data.u64 = i;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);

        fleet.states[i] = BotState::IDLE;
        connected++;
    }

    std::fprintf(stderr, "[io_engine] Connected %zu / %zu bots to %s:%u\n",
                 connected, fleet.count, config.target_host, config.target_port);

    return connected > 0;
}

std::size_t EpollIoEngine::run_batch(
    BotFleet& fleet,
    SPSCRingBuffer<LatencySample, 1048576>& telemetry_ring) noexcept {

    // Phase 1: Send payloads for IDLE bots
    std::size_t sends_this_batch = 0;
    for (std::size_t i = 0; i < fleet.count && sends_this_batch < config_.batch_size; ++i) {
        if (fleet.states[i] != BotState::IDLE || fleet.socket_fds[i] < 0) continue;

        // Patch the payload with current seq_num and TSC
        const uint64_t tsc_now = rdtsc();
        const uint32_t seq = next_seq_++;
        patch_payload(fleet.payloads[i], seq, tsc_now);

        fleet.sequence_nums[i] = seq;
        fleet.send_tsc[i] = tsc_now;

        ssize_t sent = ::send(fleet.socket_fds[i], fleet.payloads[i],
                              fleet.payload_lens[i], MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent > 0) {
            fleet.states[i] = BotState::WAITING;
            total_sends_++;
            sends_this_batch++;
        }
    }

    // Phase 2: Poll for ACKs
    struct epoll_event events[256];
    int nfds = ::epoll_wait(epoll_fd_, events, 256, 0 /* non-blocking */);

    std::size_t recvs_this_batch = 0;
    for (int n = 0; n < nfds; ++n) {
        if (!(events[n].events & EPOLLIN)) continue;

        const std::size_t bot_idx = events[n].data.u64;
        if (bot_idx >= fleet.count) continue;

        AckMessage ack{};
        ssize_t received = ::recv(fleet.socket_fds[bot_idx], &ack, sizeof(ack), MSG_DONTWAIT);
        if (received == sizeof(AckMessage) && ack.magic == 0x41434B00) {
            const uint64_t recv_tsc = rdtsc();
            fleet.recv_tsc[bot_idx] = recv_tsc;
            fleet.states[bot_idx] = BotState::IDLE;
            total_recvs_++;
            recvs_this_batch++;

            // Push latency sample into telemetry ring buffer
            LatencySample sample{
                .send_tsc = fleet.send_tsc[bot_idx],
                .recv_tsc = recv_tsc,
                .bot_id   = fleet.bot_ids[bot_idx],
                .seq_num  = ack.seq_num,
            };
            (void)telemetry_ring.try_push(sample);
        }
    }

    return recvs_this_batch;
}

void EpollIoEngine::shutdown() noexcept {
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }
}

// =============================================================================
// Factory
// =============================================================================

IoEngine* create_io_engine(HugePageArena& arena) noexcept {
    // For now, always use epoll. io_uring engine will be added when
    // liburing is built and linked.
    void* mem = arena.allocate_raw(sizeof(EpollIoEngine), alignof(EpollIoEngine));
    if (!mem) return nullptr;
    return new (mem) EpollIoEngine();
}

} // namespace iicpc
