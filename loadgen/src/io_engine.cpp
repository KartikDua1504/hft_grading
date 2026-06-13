// --- io_uring + epoll I/O Engine Implementation ---
// io_uring: registered buffers, fixed files, optional SQPOLL.
// epoll: fallback for systems without io_uring support.

#include "loadgen/io_engine.hpp"
#include "core/tsc.hpp"
#include "core/compiler_hints.hpp"

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

// --- Shared Helpers ---

static bool set_nonblocking(int fd) noexcept {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static int create_connected_socket(const char* host, uint16_t port) noexcept {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    // TCP_NODELAY — disable Nagle for latency
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

static void pin_cpu(int core) noexcept {
    if (core < 0) return;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    if (::sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
        std::fprintf(stderr, "[io_engine] WARNING: Failed to pin to CPU %d: %s\n",
                     core, std::strerror(errno));
    }
}

// --- Epoll Engine Implementation ---

EpollIoEngine::~EpollIoEngine() noexcept {
    shutdown();
}

bool EpollIoEngine::init(const IoEngineConfig& config,
                          BotFleet& fleet,
                          HugePageArena& arena) noexcept {
    config_ = config;
    pin_cpu(config.cpu_affinity);

    epoll_fd_ = ::epoll_create1(0);
    if (epoll_fd_ < 0) {
        std::fprintf(stderr, "[epoll] Failed to create epoll: %s\n", std::strerror(errno));
        return false;
    }

    recv_buf_ = arena.allocate<uint8_t>(sizeof(AckMessage) * config.batch_size);
    if (!recv_buf_) return false;

    std::size_t connected = 0;
    for (std::size_t i = 0; i < fleet.count; ++i) {
        int fd = create_connected_socket(config.target_host, config.target_port);
        if (fd < 0) {
            if (i < 5) {
                std::fprintf(stderr, "[epoll] Bot %zu failed to connect: %s\n",
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

    std::fprintf(stderr, "[epoll] Connected %zu / %zu bots to %s:%u\n",
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
    int nfds = ::epoll_wait(epoll_fd_, events, 256, 0);

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

// --- io_uring Engine Implementation ---

#if IICPC_HAS_URING

IoUringEngine::~IoUringEngine() noexcept {
    shutdown();
}

bool IoUringEngine::init(const IoEngineConfig& config,
                          BotFleet& fleet,
                          HugePageArena& arena) noexcept {
    config_ = config;
    pin_cpu(config.cpu_affinity);

    // --- Initialize io_uring ring ---
    struct io_uring_params params{};
    if (config.sqpoll) {
        params.flags |= IORING_SETUP_SQPOLL;
        params.sq_thread_idle = 2000; // 2ms idle before kernel thread sleeps
    }

    int ret = io_uring_queue_init_params(
        static_cast<unsigned>(config.ring_depth), &ring_, &params);

    if (ret < 0) {
        if (config.sqpoll && ret == -EPERM) {
            // SQPOLL requires CAP_SYS_NICE — retry without it
            std::fprintf(stderr, "[io_uring] SQPOLL requires CAP_SYS_NICE, falling back to regular mode\n");
            std::memset(&params, 0, sizeof(params));
            ret = io_uring_queue_init_params(
                static_cast<unsigned>(config.ring_depth), &ring_, &params);
        }
        if (ret < 0) {
            std::fprintf(stderr, "[io_uring] Failed to init ring: %s\n", std::strerror(-ret));
            return false;
        }
        sqpoll_active_ = false;
    } else {
        sqpoll_active_ = config.sqpoll && (params.features & IORING_FEAT_SQPOLL_NONFIXED);
        // Even if the flag was set, check if the kernel actually activated SQPOLL
        sqpoll_active_ = config.sqpoll; // We trust the init succeeded with SQPOLL flag
    }
    ring_initialized_ = true;

    // --- Connect all bot sockets ---
    std::size_t connected = 0;
    registered_fds_ = arena.allocate<int>(fleet.count);
    if (!registered_fds_) return false;
    num_registered_fds_ = fleet.count;

    for (std::size_t i = 0; i < fleet.count; ++i) {
        int fd = create_connected_socket(config.target_host, config.target_port);
        if (fd < 0) {
            if (i < 5) {
                std::fprintf(stderr, "[io_uring] Bot %zu failed to connect: %s\n",
                             i, std::strerror(errno));
            }
            fleet.socket_fds[i] = -1;
            registered_fds_[i] = -1;
            continue;
        }

        fleet.socket_fds[i] = fd;
        registered_fds_[i] = fd;
        fleet.states[i] = BotState::IDLE;
        connected++;
    }

    if (connected == 0) {
        std::fprintf(stderr, "[io_uring] No bots connected\n");
        return false;
    }

    // --- Register file descriptors (fixed files) ---
    ret = io_uring_register_files(&ring_, registered_fds_,
                                  static_cast<unsigned>(fleet.count));
    if (ret < 0) {
        std::fprintf(stderr, "[io_uring] WARNING: register_files failed: %s (continuing without)\n",
                     std::strerror(-ret));
        files_registered_ = false;
    } else {
        files_registered_ = true;
    }

    // --- Allocate per-bot recv buffers and register with io_uring ---
    constexpr std::size_t RECV_BUF_SIZE = 64; // AckMessage is 20B, pad to cache-line
    recv_buf_base_ = arena.allocate<uint8_t>(RECV_BUF_SIZE * fleet.count);
    if (!recv_buf_base_) return false;
    std::memset(recv_buf_base_, 0, RECV_BUF_SIZE * fleet.count);

    // Build iovec array for registered buffers
    num_iovecs_ = fleet.count;
    iovecs_ = arena.allocate<struct iovec>(fleet.count);
    if (!iovecs_) return false;

    for (std::size_t i = 0; i < fleet.count; ++i) {
        iovecs_[i].iov_base = recv_buf_base_ + (i * RECV_BUF_SIZE);
        iovecs_[i].iov_len  = RECV_BUF_SIZE;
    }

    ret = io_uring_register_buffers(&ring_, iovecs_,
                                    static_cast<unsigned>(fleet.count));
    if (ret < 0) {
        std::fprintf(stderr, "[io_uring] WARNING: register_buffers failed: %s (continuing without)\n",
                     std::strerror(-ret));
        buffers_registered_ = false;
    } else {
        buffers_registered_ = true;
    }

    std::fprintf(stderr, "[io_uring] Connected %zu / %zu bots to %s:%u\n",
                 connected, fleet.count, config.target_host, config.target_port);
    std::fprintf(stderr, "[io_uring] Mode: %s | Fixed files: %s | Registered buffers: %s\n",
                 sqpoll_active_ ? "SQPOLL" : "regular",
                 files_registered_ ? "YES" : "NO",
                 buffers_registered_ ? "YES" : "NO");

    return true;
}

void IoUringEngine::submit_recv(std::size_t bot_idx) noexcept {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (IICPC_UNLIKELY(!sqe)) return;

    constexpr std::size_t RECV_BUF_SIZE = 64;
    void* buf = recv_buf_base_ + (bot_idx * RECV_BUF_SIZE);

    if (files_registered_) {
        io_uring_prep_recv(sqe, static_cast<int>(bot_idx), buf, RECV_BUF_SIZE, 0);
        sqe->flags |= IOSQE_FIXED_FILE;
    } else {
        // Fall back to regular fd
        io_uring_prep_recv(sqe, registered_fds_[bot_idx], buf, RECV_BUF_SIZE, 0);
    }

    io_uring_sqe_set_data64(sqe, encode_recv(bot_idx));
    pending_recvs_++;
}

IICPC_HOT
std::size_t IoUringEngine::run_batch(
    BotFleet& fleet,
    SPSCRingBuffer<LatencySample, 1048576>& telemetry_ring) noexcept {

    // --- Phase 1: Submit linked send+recv SQE pairs for IDLE bots ---
    // Each pair: send payload → recv ACK (linked via IOSQE_IO_LINK).
    // The kernel sequences them: recv only starts after send completes.
    // This eliminates the second io_uring_submit() call.
    std::size_t sends_this_batch = 0;

    for (std::size_t i = 0; i < fleet.count && sends_this_batch < config_.batch_size; ++i) {
        if (fleet.states[i] != BotState::IDLE || registered_fds_[i] < 0) continue;

        // Need 2 SQEs (send + recv)
        struct io_uring_sqe* send_sqe = io_uring_get_sqe(&ring_);
        if (IICPC_UNLIKELY(!send_sqe)) break;
        struct io_uring_sqe* recv_sqe = io_uring_get_sqe(&ring_);
        if (IICPC_UNLIKELY(!recv_sqe)) break;

        // Patch payload with current seq + TSC
        const uint64_t tsc_now = rdtsc();
        const uint32_t seq = next_seq_++;
        patch_payload(fleet.payloads[i], seq, tsc_now);

        fleet.sequence_nums[i] = seq;
        fleet.send_tsc[i] = tsc_now;

        // --- Send SQE (linked to recv) ---
        if (files_registered_) {
            io_uring_prep_send(send_sqe, static_cast<int>(i),
                               fleet.payloads[i], fleet.payload_lens[i], MSG_NOSIGNAL);
            send_sqe->flags |= IOSQE_FIXED_FILE | IOSQE_IO_LINK;
        } else {
            io_uring_prep_send(send_sqe, registered_fds_[i],
                               fleet.payloads[i], fleet.payload_lens[i], MSG_NOSIGNAL);
            send_sqe->flags |= IOSQE_IO_LINK;
        }
        io_uring_sqe_set_data64(send_sqe, encode_send(i));

        // --- Recv SQE (chained after send) ---
        constexpr std::size_t RECV_BUF_SIZE = 64;
        void* buf = recv_buf_base_ + (i * RECV_BUF_SIZE);
        if (files_registered_) {
            io_uring_prep_recv(recv_sqe, static_cast<int>(i), buf, RECV_BUF_SIZE, 0);
            recv_sqe->flags |= IOSQE_FIXED_FILE;
        } else {
            io_uring_prep_recv(recv_sqe, registered_fds_[i], buf, RECV_BUF_SIZE, 0);
        }
        io_uring_sqe_set_data64(recv_sqe, encode_recv(i));

        fleet.states[i] = BotState::SENDING;
        sends_this_batch++;
    }

    // --- Single submit for all linked send+recv pairs ---
    if (sends_this_batch > 0) {
        io_uring_submit(&ring_);
    }

    // --- Phase 2: Drain completions ---
    struct io_uring_cqe* cqes[256];
    unsigned nr_cqes = io_uring_peek_batch_cqe(&ring_, cqes, 256);

    std::size_t recvs_this_batch = 0;

    for (unsigned c = 0; c < nr_cqes; ++c) {
        struct io_uring_cqe* cqe = cqes[c];
        const uint64_t ud = io_uring_cqe_get_data64(cqe);
        const std::size_t idx = bot_index(ud);

        if (idx >= fleet.count) {
            io_uring_cqe_seen(&ring_, cqe);
            continue;
        }

        if (!is_recv(ud)) {
            // --- Send completion ---
            if (cqe->res >= 0) {
                total_sends_++;
                fleet.states[idx] = BotState::WAITING;
                // recv is already queued via IO_LINK — no action needed
            } else {
                // Send failed — linked recv will be cancelled by kernel
                fleet.states[idx] = BotState::IDLE;
            }
        } else {
            // --- Recv completion ---
            if (cqe->res >= static_cast<int>(sizeof(AckMessage))) {
                constexpr std::size_t RECV_BUF_SIZE = 64;
                auto* ack = reinterpret_cast<const AckMessage*>(
                    recv_buf_base_ + (idx * RECV_BUF_SIZE));

                if (ack->magic == 0x41434B00) {
                    const uint64_t recv_tsc = rdtsc();
                    fleet.recv_tsc[idx] = recv_tsc;
                    fleet.states[idx] = BotState::IDLE;
                    total_recvs_++;
                    recvs_this_batch++;

                    LatencySample sample{
                        .send_tsc = fleet.send_tsc[idx],
                        .recv_tsc = recv_tsc,
                        .bot_id   = fleet.bot_ids[idx],
                        .seq_num  = ack->seq_num,
                    };
                    (void)telemetry_ring.try_push(sample);
                } else {
                    fleet.states[idx] = BotState::IDLE;
                }
            } else if (cqe->res == 0) {
                // Connection closed
                fleet.states[idx] = BotState::IDLE;
                registered_fds_[idx] = -1;
            } else if (cqe->res == -ECANCELED) {
                // Cancelled due to linked send failure — expected
                fleet.states[idx] = BotState::IDLE;
            } else {
                // Recv error
                fleet.states[idx] = BotState::IDLE;
            }
        }

        io_uring_cqe_seen(&ring_, cqe);
    }

    return recvs_this_batch;
}

void IoUringEngine::shutdown() noexcept {
    if (!ring_initialized_) return;

    if (files_registered_) {
        io_uring_unregister_files(&ring_);
        files_registered_ = false;
    }
    if (buffers_registered_) {
        io_uring_unregister_buffers(&ring_);
        buffers_registered_ = false;
    }

    // Close bot sockets
    if (registered_fds_) {
        for (std::size_t i = 0; i < num_registered_fds_; ++i) {
            if (registered_fds_[i] >= 0) {
                ::close(registered_fds_[i]);
                registered_fds_[i] = -1;
            }
        }
    }

    io_uring_queue_exit(&ring_);
    ring_initialized_ = false;

    std::fprintf(stderr, "[io_uring] Shutdown complete. Sent: %lu, Recv: %lu\n",
                 total_sends_, total_recvs_);
}

#endif // IICPC_HAS_URING

// --- Factory ---

IoEngine* create_io_engine(HugePageArena& arena) noexcept {
#if IICPC_HAS_URING
    // Probe io_uring support by initializing a minimal ring
    struct io_uring probe_ring{};
    int ret = io_uring_queue_init(4, &probe_ring, 0);
    if (ret >= 0) {
        io_uring_queue_exit(&probe_ring);

        void* mem = arena.allocate_raw(sizeof(IoUringEngine), alignof(IoUringEngine));
        if (mem) {
            std::fprintf(stderr, "[io_engine] io_uring available — using io_uring engine\n");
            return new (mem) IoUringEngine();
        }
    } else {
        std::fprintf(stderr, "[io_engine] io_uring probe failed (%s) — falling back to epoll\n",
                     std::strerror(-ret));
    }
#endif

    void* mem = arena.allocate_raw(sizeof(EpollIoEngine), alignof(EpollIoEngine));
    if (!mem) return nullptr;
    std::fprintf(stderr, "[io_engine] Using epoll fallback engine\n");
    return new (mem) EpollIoEngine();
}

} // namespace iicpc
