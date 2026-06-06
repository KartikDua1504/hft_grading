#pragma once
// =============================================================================
// shm_engine.hpp — Shared Memory IPC Engine (Zero Syscalls on Hot Path)
// =============================================================================
// This is the final boss. Eliminates ALL syscalls from the hot path by using
// shared-memory ring buffers for communication between loadgen and exchange.
//
// Architecture:
//   Loadgen thread → [SPSC Ring: requests] → Exchange thread
//   Exchange thread → [SPSC Ring: responses] → Loadgen thread
//
// No sockets. No syscalls. No kernel. Pure userspace.
// Expected: <3µs p50, because only costs are:
//   - rdtsc(): ~20ns
//   - atomic load/store: ~5-20ns
//   - memcpy payload: ~10ns
//   - cache line transfer between cores: ~40-80ns (L3 latency)
// =============================================================================

#include "core/types.hpp"
#include "core/tsc.hpp"
#include "core/compiler_hints.hpp"
#include "loadgen/bot_fleet.hpp"
#include "loadgen/payload_gen.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace iicpc {

// =============================================================================
// Shared-memory request/response messages
// =============================================================================
struct alignas(64) ShmRequest {
    uint32_t bot_id;
    uint32_t seq_num;
    uint64_t send_tsc;
    uint8_t  valid;     // 1 = valid, 0 = empty
    uint8_t  _pad[43];  // Pad to 64 bytes (1 cache line)
};
static_assert(sizeof(ShmRequest) == 64, "ShmRequest must be exactly 1 cache line");

struct alignas(64) ShmResponse {
    uint32_t bot_id;
    uint32_t seq_num;
    uint64_t send_tsc;  // Echo back for latency measurement
    uint8_t  valid;
    uint8_t  _pad[43];
};
static_assert(sizeof(ShmResponse) == 64, "ShmResponse must be exactly 1 cache line");

// =============================================================================
// Lock-free SPSC channel (compile-time sized, cache-line separated indices)
// =============================================================================
template<typename T, std::size_t Capacity>
class ShmChannel {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static constexpr std::size_t MASK = Capacity - 1;

public:
    ShmChannel() noexcept = default;

    void init(T* buffer) noexcept {
        buffer_ = buffer;
        write_pos_.store(0, std::memory_order_relaxed);
        read_pos_.store(0, std::memory_order_relaxed);
    }

    IICPC_FORCE_INLINE
    bool try_push(const T& item) noexcept {
        const auto w = write_pos_.load(std::memory_order_relaxed);
        const auto r = read_pos_.load(std::memory_order_acquire);

        if (IICPC_UNLIKELY(w - r >= Capacity)) return false; // Full

        buffer_[w & MASK] = item;
        compiler_barrier();
        write_pos_.store(w + 1, std::memory_order_release);
        return true;
    }

    IICPC_FORCE_INLINE
    bool try_pop(T& item) noexcept {
        const auto r = read_pos_.load(std::memory_order_relaxed);
        const auto w = write_pos_.load(std::memory_order_acquire);

        if (IICPC_UNLIKELY(r >= w)) return false; // Empty

        item = buffer_[r & MASK];
        compiler_barrier();
        read_pos_.store(r + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return write_pos_.load(std::memory_order_acquire) -
               read_pos_.load(std::memory_order_acquire);
    }

private:
    T* buffer_ = nullptr;
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> write_pos_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> read_pos_{0};
};

// =============================================================================
// Shared Memory Exchange (runs in a separate thread, no syscalls)
// =============================================================================
template<std::size_t ChannelCapacity = 65536>
class ShmExchange {
public:
    using RequestChannel = ShmChannel<ShmRequest, ChannelCapacity>;
    using ResponseChannel = ShmChannel<ShmResponse, ChannelCapacity>;

    void init(RequestChannel& req_chan, ResponseChannel& resp_chan) noexcept {
        req_chan_ = &req_chan;
        resp_chan_ = &resp_chan;
    }

    /// Process one batch of requests. Returns count processed.
    IICPC_HOT IICPC_FLATTEN
    std::size_t process_batch() noexcept {
        std::size_t count = 0;
        ShmRequest req;

        // Drain all available requests
        while (req_chan_->try_pop(req)) {
            // Build response (trivial echo — simulates exchange matching)
            ShmResponse resp;
            resp.bot_id   = req.bot_id;
            resp.seq_num  = req.seq_num;
            resp.send_tsc = req.send_tsc;
            resp.valid    = 1;

            // Push response — spin if full (backpressure)
            while (IICPC_UNLIKELY(!resp_chan_->try_push(resp))) {
                cpu_pause();
            }

            count++;
            total_processed_++;
        }
        return count;
    }

    uint64_t total_processed() const noexcept { return total_processed_; }

private:
    RequestChannel* req_chan_ = nullptr;
    ResponseChannel* resp_chan_ = nullptr;
    uint64_t total_processed_ = 0;
};

// =============================================================================
// Shared Memory Loadgen Engine (zero syscalls on hot path)
// =============================================================================
template<std::size_t ChannelCapacity = 65536>
class ShmEngine {
public:
    using RequestChannel = ShmChannel<ShmRequest, ChannelCapacity>;
    using ResponseChannel = ShmChannel<ShmResponse, ChannelCapacity>;
    using TelemetryRing = SPSCRingBuffer<LatencySample, 1048576>;

    void init(RequestChannel& req_chan, ResponseChannel& resp_chan) noexcept {
        req_chan_ = &req_chan;
        resp_chan_ = &resp_chan;
    }

    IICPC_HOT IICPC_FLATTEN
    std::size_t run_batch(BotFleet& fleet, TelemetryRing& ring) noexcept {
        // Phase 1: Send requests for IDLE bots
        send_requests(fleet);

        // Phase 2: Receive responses
        return recv_responses(fleet, ring);
    }

    /// Tight interleaved send/recv — reduces function call overhead
    /// and improves cache utilization by keeping both paths warm.
    IICPC_HOT IICPC_FLATTEN
    std::size_t run_batch_tight(BotFleet& fleet, TelemetryRing& ring) noexcept {
        std::size_t total_recvs_batch = 0;

        // Interleave: for each bot, try send then try recv
        // This keeps both channel caches warm simultaneously
        for (std::size_t i = 0; i < fleet.count; ++i) {
            // Prefetch next bot's state
            if (IICPC_LIKELY(i + 4 < fleet.count)) {
                prefetch_read_l1(&fleet.states[i + 4]);
            }

            // Try send
            if (fleet.states[i] == BotState::IDLE) {
                const uint64_t tsc_now = rdtsc();
                const uint32_t seq = next_seq_++;

                ShmRequest req;
                req.bot_id   = fleet.bot_ids[i];
                req.seq_num  = seq;
                req.send_tsc = tsc_now;
                req.valid    = 1;

                if (IICPC_LIKELY(req_chan_->try_push(req))) {
                    fleet.send_tsc[i] = tsc_now;
                    fleet.sequence_nums[i] = seq;
                    fleet.states[i] = BotState::WAITING;
                    total_sends_++;
                }
            }

            // Try recv (drain one response per iteration to stay balanced)
            ShmResponse resp;
            if (resp_chan_->try_pop(resp)) {
                const uint64_t recv_tsc = rdtsc();
                const std::size_t bot_idx = resp.bot_id;

                if (IICPC_LIKELY(bot_idx < fleet.count)) {
                    // Write-prefetch the state we're about to modify
                    prefetch_write_l1(&fleet.states[bot_idx]);

                    fleet.recv_tsc[bot_idx] = recv_tsc;
                    fleet.states[bot_idx] = BotState::IDLE;

                    const LatencySample sample{
                        .send_tsc = resp.send_tsc,
                        .recv_tsc = recv_tsc,
                        .bot_id   = static_cast<uint32_t>(bot_idx),
                        .seq_num  = resp.seq_num,
                    };
                    (void)ring.try_push(sample);
                }

                total_recvs_++;
                total_recvs_batch++;
            }
        }

        // Final drain pass for any remaining responses
        total_recvs_batch += recv_responses(fleet, ring);
        return total_recvs_batch;
    }

    [[nodiscard]] uint64_t total_sends() const noexcept { return total_sends_; }
    [[nodiscard]] uint64_t total_recvs() const noexcept { return total_recvs_; }

private:
    IICPC_HOT
    void send_requests(BotFleet& fleet) noexcept {
        for (std::size_t i = 0; i < fleet.count; ++i) {
            // Tighter prefetch: 4 ahead for L1 hit rate (64 bytes × 4 = 256 bytes)
            if (IICPC_LIKELY(i + 4 < fleet.count)) {
                prefetch_read_l1(&fleet.states[i + 4]);
                prefetch_read_l1(&fleet.bot_ids[i + 4]);
            }

            if (IICPC_UNLIKELY(fleet.states[i] != BotState::IDLE)) continue;

            const uint64_t tsc_now = rdtsc();
            const uint32_t seq = next_seq_++;

            ShmRequest req;
            req.bot_id   = fleet.bot_ids[i];
            req.seq_num  = seq;
            req.send_tsc = tsc_now;
            req.valid    = 1;

            if (IICPC_LIKELY(req_chan_->try_push(req))) {
                fleet.send_tsc[i] = tsc_now;
                fleet.sequence_nums[i] = seq;
                fleet.states[i] = BotState::WAITING;
                total_sends_++;
            }
        }
    }

    IICPC_HOT
    std::size_t recv_responses(BotFleet& fleet, TelemetryRing& ring) noexcept {
        std::size_t recvs = 0;
        ShmResponse resp;

        while (resp_chan_->try_pop(resp)) {
            const uint64_t recv_tsc = rdtsc();
            const std::size_t bot_idx = resp.bot_id;

            if (IICPC_LIKELY(bot_idx < fleet.count)) {
                // Write-prefetch: we're about to write fleet state + tsc arrays
                prefetch_write_l1(&fleet.states[bot_idx]);
                prefetch_write_l1(&fleet.recv_tsc[bot_idx]);

                fleet.recv_tsc[bot_idx] = recv_tsc;
                fleet.states[bot_idx] = BotState::IDLE;

                const LatencySample sample{
                    .send_tsc = resp.send_tsc,
                    .recv_tsc = recv_tsc,
                    .bot_id   = static_cast<uint32_t>(bot_idx),
                    .seq_num  = resp.seq_num,
                };
                (void)ring.try_push(sample);
            }

            total_recvs_++;
            recvs++;
        }
        return recvs;
    }

    RequestChannel* req_chan_ = nullptr;
    ResponseChannel* resp_chan_ = nullptr;
    uint64_t total_sends_ = 0;
    uint64_t total_recvs_ = 0;
    uint32_t next_seq_ = 0;
};

} // namespace iicpc
