#pragma once
// =============================================================================
// io_engine.hpp — io_uring / epoll Async I/O Abstraction
// =============================================================================
// The blast engine that sends pre-computed payloads over TCP.
// Primary: io_uring with SQPOLL for kernel-side submission.
// Fallback: epoll for systems without io_uring support.
// =============================================================================

#include "core/types.hpp"
#include "core/tsc.hpp"
#include "core/ring_buffer.hpp"
#include "loadgen/bot_fleet.hpp"
#include "loadgen/payload_gen.hpp"

#include <cstdint>
#include <cstdio>

#if IICPC_HAS_URING
// Forward declare — actual include in .cpp to avoid header pollution
struct io_uring;
#endif

namespace iicpc {

/// I/O engine configuration
struct IoEngineConfig {
    const char* target_host = "127.0.0.1";
    uint16_t target_port = 9999;
    std::size_t batch_size = 256;       // SQEs per io_uring_enter
    std::size_t ring_depth = 4096;      // io_uring queue depth
    std::size_t target_tps = 100'000;   // Target transactions per second
    int cpu_affinity = -1;              // Core to pin to (-1 = no pin)
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
};

/// Create the best available I/O engine for this system.
/// Returns io_uring engine if available, epoll fallback otherwise.
[[nodiscard]] IoEngine* create_io_engine(HugePageArena& arena) noexcept;

// =============================================================================
// Epoll-based fallback engine (always available)
// =============================================================================
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

private:
    IoEngineConfig config_{};
    int epoll_fd_ = -1;
    uint64_t total_sends_ = 0;
    uint64_t total_recvs_ = 0;
    uint32_t next_seq_ = 0;
    // Receive buffer for ACKs
    uint8_t* recv_buf_ = nullptr;
};

} // namespace iicpc
