#pragma once

// --- Deterministic Order Sequencer ---
// Assigns strictly monotonic sequence numbers to all incoming orders.
// Guarantees fairness and deterministic replay:
//   Same sequence → same fills, always.
//
// Architecture: Gateway threads → stamp() → MPSC ring → drain() → MatchEngine
// Software mode: lock-free atomic<uint64_t> counter.

#include "sdk/protocol.hpp"
#include "core/arena.hpp"
#include "core/compiler_hints.hpp"
#include "core/hot_path_asm.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace iicpc {

// --- Sequenced Order (what the match engine processes) ---
struct alignas(64) SequencedOrder {
    uint64_t    sequence_no;        // Global monotonic sequence number
    uint64_t    recv_tsc;           // TSC when gateway received order
    uint32_t    contestant_id;
    MsgType     msg_type;           // ORDER_ENTRY or CANCEL_REQUEST
    uint8_t     _pad[3];
    union {
        OrderEntry    order;        // 64 bytes
        CancelRequest cancel;       // 32 bytes (padded in union)
    };
};
static_assert(sizeof(SequencedOrder) == 128 || sizeof(SequencedOrder) == 192,
              "SequencedOrder should be cache-line aligned");

// --- Sequencer Config ---
struct SequencerConfig {
    uint32_t ring_size = 65536;     // Must be power of 2
    bool     enable_tsc = true;
};

// --- Lock-Free MPSC Sequencer ---
// Producers (gateways) call stamp() concurrently.
// Consumer (match engine) calls drain() on a single thread.
inline constexpr uint32_t SEQ_RING_SIZE = 65536;
inline constexpr uint32_t SEQ_RING_MASK = SEQ_RING_SIZE - 1;

class OrderSequencer {
public:
    OrderSequencer() noexcept = default;

    // --- Initialize: allocate ring from arena ---
    bool init(HugePageArena& arena) noexcept {
        ring_ = static_cast<SequencedOrder*>(
            arena.allocate_raw(sizeof(SequencedOrder) * SEQ_RING_SIZE, 64));
        if (!ring_) return false;

        std::memset(ring_, 0, sizeof(SequencedOrder) * SEQ_RING_SIZE);

        // Committed flags: 0 = empty, 1 = ready, 2 = being written
        committed_ = static_cast<std::atomic<uint8_t>*>(
            arena.allocate_raw(sizeof(std::atomic<uint8_t>) * SEQ_RING_SIZE, 64));
        if (!committed_) return false;
        for (uint32_t i = 0; i < SEQ_RING_SIZE; ++i) {
            committed_[i].store(0, std::memory_order_relaxed);
        }

        seq_counter_.store(0, std::memory_order_relaxed);
        drain_pos_ = 0;
        total_stamped_.store(0, std::memory_order_relaxed);
        total_drained_.store(0, std::memory_order_relaxed);
        drops_.store(0, std::memory_order_relaxed);

        std::fprintf(stderr, "[sequencer] Initialized: %u slots, lock-free MPSC\n",
                     SEQ_RING_SIZE);
        return true;
    }

    // --- stamp() — concurrent, lock-free (called by gateways) ---
    // Returns assigned sequence number, or 0 on ring-full drop.
    IICPC_HOT
    uint64_t stamp_order(uint32_t contestant_id,
                         const OrderEntry& order) noexcept {
        uint64_t seq = seq_counter_.fetch_add(1, std::memory_order_acq_rel) + 1;
        uint32_t slot = static_cast<uint32_t>(seq) & SEQ_RING_MASK;

        uint8_t expected = 0;
        if (!committed_[slot].compare_exchange_strong(expected, 2,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            drops_.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }

        SequencedOrder& so = ring_[slot];
        so.sequence_no = seq;
        so.recv_tsc = asm_hot::rdtsc_serialized();
        so.contestant_id = contestant_id;
        so.msg_type = MsgType::ORDER_ENTRY;
        std::memcpy(&so.order, &order, sizeof(OrderEntry));

        committed_[slot].store(1, std::memory_order_release);
        total_stamped_.fetch_add(1, std::memory_order_relaxed);

        return seq;
    }

    IICPC_HOT
    uint64_t stamp_cancel(uint32_t contestant_id,
                          const CancelRequest& cancel) noexcept {
        uint64_t seq = seq_counter_.fetch_add(1, std::memory_order_acq_rel) + 1;
        uint32_t slot = static_cast<uint32_t>(seq) & SEQ_RING_MASK;

        uint8_t expected = 0;
        if (!committed_[slot].compare_exchange_strong(expected, 2,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            drops_.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }

        SequencedOrder& so = ring_[slot];
        so.sequence_no = seq;
        so.recv_tsc = asm_hot::rdtsc_serialized();
        so.contestant_id = contestant_id;
        so.msg_type = MsgType::CANCEL_REQUEST;
        std::memcpy(&so.cancel, &cancel, sizeof(CancelRequest));

        committed_[slot].store(1, std::memory_order_release);
        total_stamped_.fetch_add(1, std::memory_order_relaxed);
        return seq;
    }

    // --- drain() — single-thread consumer (match engine) ---
    IICPC_HOT
    bool drain(SequencedOrder& out) noexcept {
        uint32_t slot = static_cast<uint32_t>(drain_pos_ + 1) & SEQ_RING_MASK;

        if (committed_[slot].load(std::memory_order_acquire) != 1) {
            return false;
        }

        std::memcpy(&out, &ring_[slot], sizeof(SequencedOrder));

        committed_[slot].store(0, std::memory_order_release);
        drain_pos_++;
        total_drained_.fetch_add(1, std::memory_order_relaxed);

        return true;
    }

    // --- Batch drain ---
    uint32_t drain_batch(SequencedOrder* out, uint32_t max_batch) noexcept {
        uint32_t count = 0;
        while (count < max_batch && drain(out[count])) {
            ++count;
        }
        return count;
    }

    // --- Stats ---
    [[nodiscard]] uint64_t total_stamped() const noexcept {
        return total_stamped_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t total_drained() const noexcept {
        return total_drained_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t drops() const noexcept {
        return drops_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t current_seq() const noexcept {
        return seq_counter_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t pending() const noexcept {
        return total_stamped() - total_drained();
    }

    void print_stats() const noexcept {
        std::fprintf(stderr,
            "[sequencer] Stats: stamped=%lu drained=%lu drops=%lu pending=%lu\n",
            total_stamped(), total_drained(), drops(), pending());
    }

private:
    SequencedOrder*          ring_ = nullptr;
    std::atomic<uint8_t>*    committed_ = nullptr;

    // Producer side (shared, contended)
    alignas(64) std::atomic<uint64_t> seq_counter_{0};
    alignas(64) std::atomic<uint64_t> total_stamped_{0};
    alignas(64) std::atomic<uint64_t> drops_{0};

    // Consumer side (single thread)
    alignas(64) uint64_t drain_pos_ = 0;
    alignas(64) std::atomic<uint64_t> total_drained_{0};
};

} // namespace iicpc
