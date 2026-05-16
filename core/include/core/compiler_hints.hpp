#pragma once
// =============================================================================
// compiler_hints.hpp — Branch prediction, prefetching, and compiler intrinsics
// =============================================================================
// Every hint here has measurable impact on the hot path.
// These are the difference between "low-level code" and "engineered performance."
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <x86intrin.h>

namespace iicpc {

// --- Branch prediction hints ---
// GCC/Clang __builtin_expect. On Alder Lake P-cores, a mispredicted branch
// costs ~15 cycles. A well-predicted branch costs ~1 cycle.
#define IICPC_LIKELY(x)   __builtin_expect(!!(x), 1)
#define IICPC_UNLIKELY(x) __builtin_expect(!!(x), 0)

// --- Prefetch hints ---
// L1 prefetch: 64 bytes. On Alder Lake, L1 latency ~4 cycles, L2 ~12, L3 ~40.
// Prefetching saves the delta.

/// Prefetch for read, into L1 cache (highest priority)
inline void prefetch_read_l1(const void* addr) noexcept {
    __builtin_prefetch(addr, 0, 3);  // read, high temporal locality
}

/// Prefetch for read, into L2 cache
inline void prefetch_read_l2(const void* addr) noexcept {
    __builtin_prefetch(addr, 0, 2);
}

/// Prefetch for write (exclusive access), into L1
inline void prefetch_write_l1(const void* addr) noexcept {
    __builtin_prefetch(addr, 1, 3);
}

/// Prefetch a range of memory (for SoA array traversal)
template<std::size_t Stride = 64>
inline void prefetch_range(const void* addr, std::size_t bytes) noexcept {
    const auto* p = static_cast<const char*>(addr);
    for (std::size_t i = 0; i < bytes; i += Stride) {
        __builtin_prefetch(p + i, 0, 3);
    }
}

/// Prefetch N cache lines ahead in an SoA array
template<typename T, std::size_t Ahead = 8>
inline void prefetch_soa_ahead(const T* arr, std::size_t current_idx) noexcept {
    const std::size_t prefetch_idx = current_idx + Ahead;
    __builtin_prefetch(&arr[prefetch_idx], 0, 3);
}

// --- CPU yield hints ---
/// Spin-wait hint (saves power, reduces memory bus contention)
inline void cpu_pause() noexcept {
    _mm_pause();
}

/// Spin with backoff (avoids saturating the memory bus)
inline void spin_wait(unsigned cycles) noexcept {
    for (unsigned i = 0; i < cycles; ++i) {
        _mm_pause();
    }
}

// --- Memory barriers (lighter than atomics) ---
/// Compiler-only barrier (no CPU fence, just prevents reordering)
inline void compiler_barrier() noexcept {
    asm volatile("" ::: "memory");
}

/// Store fence (ensures all prior stores are visible)
inline void store_fence() noexcept {
    _mm_sfence();
}

/// Load fence (ensures all prior loads are completed)  
inline void load_fence() noexcept {
    _mm_lfence();
}

// --- Unreachable hint ---
[[noreturn]] inline void unreachable() noexcept {
    __builtin_unreachable();
}

// --- Assume alignment (helps auto-vectorization) ---
template<std::size_t Align, typename T>
inline T* assume_aligned(T* ptr) noexcept {
    return static_cast<T*>(__builtin_assume_aligned(ptr, Align));
}

// --- Force inline for critical hot-path functions ---
#define IICPC_FORCE_INLINE __attribute__((always_inline)) inline
#define IICPC_NOINLINE     __attribute__((noinline))
#define IICPC_HOT          __attribute__((hot))
#define IICPC_COLD         __attribute__((cold))
#define IICPC_FLATTEN      __attribute__((flatten))

// --- Restrict pointer (C99 restrict for C++) ---
#define IICPC_RESTRICT __restrict__

} // namespace iicpc
