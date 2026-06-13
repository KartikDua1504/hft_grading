#pragma once

// --- Cache-Line-Aligned Ticket Spinlock + Sequence Lock ---
// Userspace-only synchronization primitives for the hot path.
// No syscalls, no context switches, deterministic latency.
//
// Hierarchy:
//   1. Lock-free (preferred) — atomics with acquire/release
//   2. Spinlock (when mutation needs exclusion) — ticket-based, fair
//   3. SeqLock (reader-writer, single writer) — zero cost for readers

#include "core/types.hpp"
#include "core/compiler_hints.hpp"

#include <atomic>
#include <cstdint>

namespace iicpc {

// --- Ticket Spinlock ---
// Fair, FIFO ordering. Each waiter gets a monotonic ticket, served in order.
// Prevents starvation unlike test-and-set.
class alignas(CACHE_LINE_SIZE) TicketSpinlock {
public:
    TicketSpinlock() noexcept = default;

    // Non-copyable
    TicketSpinlock(const TicketSpinlock&) = delete;
    TicketSpinlock& operator=(const TicketSpinlock&) = delete;

    IICPC_FORCE_INLINE
    void lock() noexcept {
        const uint32_t my_ticket = next_ticket_.fetch_add(1,
            std::memory_order_relaxed);
        while (now_serving_.load(std::memory_order_acquire) != my_ticket) {
            cpu_pause(); // Reduce bus contention while spinning
        }
    }

    IICPC_FORCE_INLINE
    void unlock() noexcept {
        now_serving_.fetch_add(1, std::memory_order_release);
    }

    [[nodiscard]] IICPC_FORCE_INLINE
    bool try_lock() noexcept {
        uint32_t expected = now_serving_.load(std::memory_order_relaxed);
        return next_ticket_.compare_exchange_strong(expected,
            expected + 1, std::memory_order_acquire,
            std::memory_order_relaxed);
    }

    /// RAII guard
    class Guard {
    public:
        explicit Guard(TicketSpinlock& lock) noexcept : lock_(lock) {
            lock_.lock();
        }
        ~Guard() noexcept { lock_.unlock(); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    private:
        TicketSpinlock& lock_;
    };

private:
    alignas(CACHE_LINE_SIZE) std::atomic<uint32_t> next_ticket_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<uint32_t> now_serving_{0};
};
static_assert(sizeof(TicketSpinlock) == 2 * CACHE_LINE_SIZE);

// --- SeqLock — Single-Writer, Multiple-Reader ---
// Writers increment sequence number before and after mutation (odd = writing).
// Readers retry if sequence changed or is odd.
// Zero cost for readers (no atomic RMW).
class alignas(CACHE_LINE_SIZE) SeqLock {
public:
    SeqLock() noexcept = default;

    /// Writer: begin write (sequence goes odd)
    IICPC_FORCE_INLINE
    void write_lock() noexcept {
        seq_.store(seq_.load(std::memory_order_relaxed) + 1,
                   std::memory_order_release);
        compiler_barrier();
    }

    /// Writer: end write (sequence goes even)
    IICPC_FORCE_INLINE
    void write_unlock() noexcept {
        compiler_barrier();
        seq_.store(seq_.load(std::memory_order_relaxed) + 1,
                   std::memory_order_release);
    }

    /// Reader: get current sequence (call before reading data)
    [[nodiscard]] IICPC_FORCE_INLINE
    uint32_t read_begin() const noexcept {
        uint32_t seq;
        do {
            seq = seq_.load(std::memory_order_acquire);
        } while (seq & 1); // Retry if writer is active (odd)
        return seq;
    }

    /// Reader: validate read (retry if returns false)
    [[nodiscard]] IICPC_FORCE_INLINE
    bool read_valid(uint32_t start_seq) const noexcept {
        compiler_barrier();
        return seq_.load(std::memory_order_acquire) == start_seq;
    }

private:
    alignas(CACHE_LINE_SIZE) std::atomic<uint32_t> seq_{0};
};

// --- MPSCQueue — Multi-Producer Single-Consumer Lock-Free Queue ---
// Aggregates results from multiple producer threads to a single consumer.
// Uses atomic linked-list with intrusive nodes. Zero allocation.
template<typename T, uint32_t Capacity>
class MPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static constexpr uint32_t MASK = Capacity - 1;

public:
    MPSCQueue() noexcept = default;

    void init() noexcept {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    /// Producer: try push (multiple threads may call concurrently)
    [[nodiscard]] IICPC_FORCE_INLINE
    bool try_push(const T& item) noexcept {
        uint32_t cur_head = head_.load(std::memory_order_relaxed);
        uint32_t next_head;
        do {
            next_head = (cur_head + 1) & MASK;
            uint32_t cur_tail = tail_.load(std::memory_order_acquire);
            if (next_head == cur_tail) return false; // Full
        } while (!head_.compare_exchange_weak(cur_head, next_head,
                     std::memory_order_acq_rel,
                     std::memory_order_relaxed));

        buffer_[cur_head] = item;
        // Signal that slot is filled
        filled_[cur_head].store(true, std::memory_order_release);
        return true;
    }

    /// Consumer: try pop (only ONE thread may call)
    [[nodiscard]] IICPC_FORCE_INLINE
    bool try_pop(T& item) noexcept {
        uint32_t cur_tail = tail_.load(std::memory_order_relaxed);
        if (cur_tail == head_.load(std::memory_order_acquire)) return false;

        // Wait for slot to be filled
        while (!filled_[cur_tail].load(std::memory_order_acquire)) {
            cpu_pause();
        }

        item = buffer_[cur_tail];
        filled_[cur_tail].store(false, std::memory_order_release);
        tail_.store((cur_tail + 1) & MASK, std::memory_order_release);
        return true;
    }

private:
    alignas(CACHE_LINE_SIZE) std::atomic<uint32_t> head_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<uint32_t> tail_{0};
    alignas(CACHE_LINE_SIZE) T buffer_[Capacity];
    std::atomic<bool> filled_[Capacity] = {};
};

} // namespace iicpc
