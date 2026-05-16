#pragma once
// =============================================================================
// types.hpp — Cache-line aligned primitives and SoA helpers
// =============================================================================
// The foundational type system. Every struct here is designed to be
// mechanically sympathetic to the Intel Alder Lake cache hierarchy:
//   L1d: 48 KiB/core (P-core), 64-byte lines
//   L2:  1.25 MiB/P-core
//   L3:  24 MiB shared
// =============================================================================

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace iicpc {

// --- Hardware Constants (compile-time) ---

/// Intel cache line size. This is the atomic unit of memory the CPU fetches.
/// False sharing occurs when two threads write to the same cache line.
inline constexpr std::size_t CACHE_LINE_SIZE = 64;

/// Huge page size (2 MiB). Used for TLB-friendly arena allocations.
inline constexpr std::size_t HUGE_PAGE_SIZE = 2 * 1024 * 1024;

/// Default arena size: 2 GiB as specified.
inline constexpr std::size_t DEFAULT_ARENA_SIZE = 2ULL * 1024 * 1024 * 1024;

/// Maximum bots in the fleet. Power of 2 is not required here but SoA arrays
/// are sized to this.
inline constexpr std::size_t MAX_BOTS = 131072; // 128k, > 100k target

// --- Cache-Line Padded Atomic ---
// Wraps an atomic value in its own cache line. This is the canonical
// false-sharing prevention pattern. Used for SPSC ring buffer indices.

template<typename T>
struct alignas(CACHE_LINE_SIZE) PaddedAtomic {
    std::atomic<T> value{0};

    // Intentional padding to fill the cache line
    // sizeof(atomic<T>) is typically 8 bytes, padding fills remaining 56
    char _pad[CACHE_LINE_SIZE - sizeof(std::atomic<T>)];

    // Convenience accessors preserving memory order semantics
    T load(std::memory_order order = std::memory_order_seq_cst) const noexcept {
        return value.load(order);
    }

    void store(T val, std::memory_order order = std::memory_order_seq_cst) noexcept {
        value.store(val, order);
    }

    T fetch_add(T val, std::memory_order order = std::memory_order_seq_cst) noexcept {
        return value.fetch_add(val, order);
    }
};

// Compile-time verification: each PaddedAtomic occupies exactly one cache line
static_assert(sizeof(PaddedAtomic<uint64_t>) == CACHE_LINE_SIZE,
    "PaddedAtomic must be exactly one cache line (64 bytes)");
static_assert(alignof(PaddedAtomic<uint64_t>) == CACHE_LINE_SIZE,
    "PaddedAtomic must be aligned to cache line boundary");

// --- Cache-Line Aligned Storage ---
// For non-atomic data that still needs cache-line alignment (e.g., per-thread
// counters, SoA array heads).

template<typename T>
struct alignas(CACHE_LINE_SIZE) CacheAligned {
    T value{};
    char _pad[CACHE_LINE_SIZE - sizeof(T)];
};

static_assert(sizeof(CacheAligned<uint64_t>) == CACHE_LINE_SIZE);

// --- Timestamp type ---
// Raw TSC ticks. Never converted on the hot path.
using TscTicks = uint64_t;

// --- Latency sample pushed through ring buffer ---
struct alignas(8) LatencySample {
    TscTicks send_tsc;    // TSC at send
    TscTicks recv_tsc;    // TSC at receive
    uint32_t bot_id;      // Which bot generated this
    uint32_t seq_num;     // Sequence number for drop detection
};

static_assert(sizeof(LatencySample) == 24, "LatencySample must be tightly packed");
static_assert(std::is_trivially_copyable_v<LatencySample>,
    "LatencySample must be trivially copyable for lock-free ring buffer");

// --- Concept constraints for ring buffer element types ---
template<typename T>
concept RingBufferElement = std::is_trivially_copyable_v<T>
                         && std::is_trivially_destructible_v<T>
                         && (sizeof(T) <= 256); // Sanity: don't put huge things in the ring

// --- Utility: Align up to boundary ---
constexpr std::size_t align_up(std::size_t value, std::size_t alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

// --- Utility: Check power of 2 ---
constexpr bool is_power_of_2(std::size_t n) noexcept {
    return n > 0 && (n & (n - 1)) == 0;
}

} // namespace iicpc
