#pragma once
// --- io_uring / epoll Async I/O Engine ---
// Transport engine for sending pre-computed payloads over TCP.
// Primary: io_uring with SQPOLL + registered buffers + fixed files.
// Fallback: epoll for systems without io_uring support.

#include "core/types.hpp"
#include "core/tsc.hpp"
#include "core/ring_buffer.hpp"
#include "loadgen/bot_fleet.hpp"
#include "loadgen/payload_gen.hpp"

#include <cstdint>
#include <cstdio>

#if IICPC_HAS_URING
#include <liburing.h>
#endif

namespace iicpc {

/// I/O engine configuration
struct IoEngineConfig {
    const char* target_host = "127.0.0.1";
    uint16_t target_port = 9999;
    std::size_t batch_size = 256;       // SQEs per submission batch
    std::size_t ring_depth = 4096;      // io_uring SQ/CQ depth
    std::size_t target_tps = 100'000;   // Target transactions per second
    int cpu_affinity = -1;              // Core to pin to (-1 = no pin)
    bool sqpoll = true;                 // Use SQPOLL kernel thread (requires CAP_SYS_NICE)
};

/// Abstract I/O engine interface
class IoEngine {
public:
    virtual ~IoEngine() noexcept = default;

    /// Initialize the engine, connect sockets, register buffers.
    [[nodiscard]] virtual bool init(const IoEngineConfig& config,
                                     BotFleet& fleet,
                                     HugePageArena& arena) noexcept = 0;

    /// Run one batch cycle: send payloads, collect completions, push latency samples.
    /// Returns number of completions (acks received) in this cycle.
    virtual std::size_t run_batch(
        BotFleet& fleet,
        SPSCRingBuffer<LatencySample, 1048576>& telemetry_ring) noexcept = 0;

    /// Shutdown and cleanup
    virtual void shutdown() noexcept = 0;

    /// Get total sends and receives
    [[nodiscard]] virtual uint64_t total_sends() const noexcept = 0;
    [[nodiscard]] virtual uint64_t total_recvs() const noexcept = 0;

    /// Engine name for logging
    [[nodiscard]] virtual const char* name() const noexcept = 0;
};

/// Create the best available I/O engine for this system.
/// Returns io_uring engine if available, epoll fallback otherwise.
[[nodiscard]] IoEngine* create_io_engine(HugePageArena& arena) noexcept;

// --- Epoll Fallback Engine ---

class EpollIoEngine : public IoEngine {
public:
    EpollIoEngine() noexcept = default;
    ~EpollIoEngine() noexcept override;

    [[nodiscard]] bool init(const IoEngineConfig& config,
                            BotFleet& fleet,
                            HugePageArena& arena) noexcept override;

    std::size_t run_batch(
        BotFleet& fleet,
        SPSCRingBuffer<LatencySample, 1048576>& telemetry_ring) noexcept override;

    void shutdown() noexcept override;

    [[nodiscard]] uint64_t total_sends() const noexcept override { return total_sends_; }
    [[nodiscard]] uint64_t total_recvs() const noexcept override { return total_recvs_; }
    [[nodiscard]] const char* name() const noexcept override { return "epoll"; }

private:
    IoEngineConfig config_{};
    int epoll_fd_ = -1;
    uint64_t total_sends_ = 0;
    uint64_t total_recvs_ = 0;
    uint32_t next_seq_ = 0;
    uint8_t* recv_buf_ = nullptr;
};

// --- io_uring Engine ---

#if IICPC_HAS_URING

class IoUringEngine : public IoEngine {
public:
    IoUringEngine() noexcept = default;
    ~IoUringEngine() noexcept override;

    [[nodiscard]] bool init(const IoEngineConfig& config,
                            BotFleet& fleet,
                            HugePageArena& arena) noexcept override;

    std::size_t run_batch(
        BotFleet& fleet,
        SPSCRingBuffer<LatencySample, 1048576>& telemetry_ring) noexcept override;

    void shutdown() noexcept override;

    [[nodiscard]] uint64_t total_sends() const noexcept override { return total_sends_; }
    [[nodiscard]] uint64_t total_recvs() const noexcept override { return total_recvs_; }
    [[nodiscard]] const char* name() const noexcept override { return sqpoll_active_ ? "io_uring (SQPOLL)" : "io_uring"; }

private:
    // --- user_data encoding ---
    // Bit 0: 0 = send completion, 1 = recv completion
    // Bits 1..31: bot index
    static constexpr uint64_t encode_send(std::size_t bot_idx) noexcept {
        return (static_cast<uint64_t>(bot_idx) << 1) | 0;
    }
    static constexpr uint64_t encode_recv(std::size_t bot_idx) noexcept {
        return (static_cast<uint64_t>(bot_idx) << 1) | 1;
    }
    static constexpr bool is_recv(uint64_t user_data) noexcept {
        return (user_data & 1) != 0;
    }
    static constexpr std::size_t bot_index(uint64_t user_data) noexcept {
        return static_cast<std::size_t>(user_data >> 1);
    }

    /// Submit a recv SQE for the given bot to receive an ACK.
    void submit_recv(std::size_t bot_idx) noexcept;

    IoEngineConfig config_{};
    struct io_uring ring_{};
    bool ring_initialized_ = false;
    bool sqpoll_active_ = false;
    bool files_registered_ = false;
    bool buffers_registered_ = false;

    // Registered file descriptor table (indexed by bot)
    int* registered_fds_ = nullptr;
    std::size_t num_registered_fds_ = 0;

    // Per-bot recv buffers (arena-allocated, registered with io_uring)
    uint8_t* recv_buf_base_ = nullptr;
    struct iovec* iovecs_ = nullptr;
    std::size_t num_iovecs_ = 0;

    uint64_t total_sends_ = 0;
    uint64_t total_recvs_ = 0;
    uint32_t next_seq_ = 0;

    // Pending recv count (to avoid double-submitting)
    uint32_t pending_recvs_ = 0;
};

#endif // IICPC_HAS_URING

} // namespace iicpc
