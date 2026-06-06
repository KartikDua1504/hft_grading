#pragma once
// =============================================================================
// compiler_hints.hpp — Branch prediction, prefetching, SIMD, and intrinsics
// =============================================================================
// Every hint here has measurable impact on the hot path.
// These are the difference between "low-level code" and "engineered performance."
//
// SIMD: We provide AVX2 and AVX-512 wrappers for:
//   - Parallel key comparison (hash map probing)
//   - Vectorized memcpy (cache-line copies)
//   - Batch zero/fill operations
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <x86intrin.h>
#include <immintrin.h>

namespace iicpc {

// --- Branch prediction hints ---
// GCC/Clang __builtin_expect. On Alder Lake P-cores, a mispredicted branch
// costs ~15 cycles. A well-predicted branch costs ~1 cycle.
#define IICPC_LIKELY(x)   __builtin_expect(!!(x), 1)
#define IICPC_UNLIKELY(x) __builtin_expect(!!(x), 0)

// --- Assumption hint (eliminates dead branches entirely) ---
#define IICPC_ASSUME(expr) do { if (!(expr)) __builtin_unreachable(); } while(0)

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

/// Prefetch for write, into L2 (for arrays we'll write to soon)
inline void prefetch_write_l2(const void* addr) noexcept {
    __builtin_prefetch(addr, 1, 2);
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

// --- Tail call optimization hint (GCC 14+, Clang 13+) ---
#if __has_attribute(musttail)
#define IICPC_MUSTTAIL [[clang::musttail]]
#else
#define IICPC_MUSTTAIL
#endif

// =============================================================================
// AVX2 SIMD Helpers — 256-bit (32 bytes) vectorized operations
// =============================================================================
// These provide massive speedups for hash map probing and data movement.
// On Alder Lake P-cores, AVX2 instructions execute at 1-cycle throughput
// on port 0/1, processing 4x int64 or 8x int32 per instruction.
//
// __attribute__((target("avx2"))) enables AVX2 per-function even when the
// translation unit is compiled without -march=native. Required for targets
// like exchange_local and run_contest that don't use iicpc_target_hotpath().
// =============================================================================

#define IICPC_AVX2_TARGET __attribute__((target("avx2")))

/// Compare 4 × int64 keys against a broadcast search key.
/// Returns a bitmask where bit N is set if keys[N] == search_key.
/// Used for 4-way parallel hash map probing — replaces 4 sequential compares.
IICPC_FORCE_INLINE IICPC_AVX2_TARGET
uint32_t avx2_find_key_i64(const int64_t* keys, int64_t search_key) noexcept {
    __m256i vkeys = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(keys));
    __m256i vtarget = _mm256_set1_epi64x(search_key);
    __m256i vcmp = _mm256_cmpeq_epi64(vkeys, vtarget);
    return static_cast<uint32_t>(_mm256_movemask_epi8(vcmp));
}

/// Check if any of 4 × int64 keys match. Returns true on hit.
IICPC_FORCE_INLINE IICPC_AVX2_TARGET
bool avx2_contains_key_i64(const int64_t* keys, int64_t search_key) noexcept {
    return avx2_find_key_i64(keys, search_key) != 0;
}

/// Extract the index of the first matching key from the bitmask.
/// Precondition: mask != 0 (at least one match).
IICPC_FORCE_INLINE
uint32_t avx2_first_match_index_i64(uint32_t mask) noexcept {
    // Each int64 occupies 8 bytes → bit positions 0-7, 8-15, 16-23, 24-31
    // __builtin_ctz gives first set bit, divide by 8 for element index
    return static_cast<uint32_t>(__builtin_ctz(mask)) >> 3;
}

/// Compare 8 × int32 keys against a broadcast search key.
/// Returns bitmask of matching elements.
IICPC_FORCE_INLINE IICPC_AVX2_TARGET
uint32_t avx2_find_key_i32(const int32_t* keys, int32_t search_key) noexcept {
    __m256i vkeys = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(keys));
    __m256i vtarget = _mm256_set1_epi32(search_key);
    __m256i vcmp = _mm256_cmpeq_epi32(vkeys, vtarget);
    return static_cast<uint32_t>(_mm256_movemask_epi8(vcmp));
}

/// Extract first match index from int32 mask.
IICPC_FORCE_INLINE
uint32_t avx2_first_match_index_i32(uint32_t mask) noexcept {
    return static_cast<uint32_t>(__builtin_ctz(mask)) >> 2;
}

/// Check 4 × uint8 distance values for zero (empty slot detection).
/// Returns bitmask of zero elements. Processes 32 bytes at once.
IICPC_FORCE_INLINE IICPC_AVX2_TARGET
uint32_t avx2_find_zero_u8(const uint8_t* data) noexcept {
    __m256i vdata = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data));
    __m256i vzero = _mm256_setzero_si256();
    __m256i vcmp = _mm256_cmpeq_epi8(vdata, vzero);
    return static_cast<uint32_t>(_mm256_movemask_epi8(vcmp));
}

/// Vectorized cache-line copy (64 bytes) using AVX2.
/// Both src and dst must be 32-byte aligned for best performance.
/// Uses non-temporal stores to avoid polluting cache for write-once data.
IICPC_FORCE_INLINE IICPC_AVX2_TARGET
void avx2_copy_cacheline(void* IICPC_RESTRICT dst,
                          const void* IICPC_RESTRICT src) noexcept {
    const auto* s = static_cast<const __m256i*>(src);
    auto* d = static_cast<__m256i*>(dst);
    __m256i a = _mm256_load_si256(s);
    __m256i b = _mm256_load_si256(s + 1);
    _mm256_store_si256(d, a);
    _mm256_store_si256(d + 1, b);
}

/// Vectorized cache-line copy with non-temporal (streaming) stores.
/// Bypasses cache — use when writing data that won't be read again soon.
IICPC_FORCE_INLINE IICPC_AVX2_TARGET
void avx2_stream_cacheline(void* IICPC_RESTRICT dst,
                            const void* IICPC_RESTRICT src) noexcept {
    const auto* s = static_cast<const __m256i*>(src);
    auto* d = static_cast<__m256i*>(dst);
    __m256i a = _mm256_load_si256(s);
    __m256i b = _mm256_load_si256(s + 1);
    _mm256_stream_si256(d, a);
    _mm256_stream_si256(d + 1, b);
}

/// Vectorized batch comparison: find price in sorted SoA array.
/// Scans `count` elements in chunks of 4, returns index or UINT32_MAX.
IICPC_FORCE_INLINE IICPC_AVX2_TARGET
uint32_t avx2_linear_search_i64(const int64_t* arr, std::size_t count,
                                 int64_t target) noexcept {
    const __m256i vtarget = _mm256_set1_epi64x(target);
    std::size_t i = 0;

    // Process 4 elements per iteration
    for (; i + 4 <= count; i += 4) {
        __m256i vdata = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(arr + i));
        __m256i vcmp = _mm256_cmpeq_epi64(vdata, vtarget);
        uint32_t mask = static_cast<uint32_t>(_mm256_movemask_epi8(vcmp));
        if (mask != 0) {
            return static_cast<uint32_t>(i) + (static_cast<uint32_t>(__builtin_ctz(mask)) >> 3);
        }
    }

    // Scalar tail
    for (; i < count; ++i) {
        if (arr[i] == target) return static_cast<uint32_t>(i);
    }
    return UINT32_MAX;
}

/// AVX2 batch zero: zero out N cache lines (N * 64 bytes).
/// Uses non-temporal stores to avoid cache pollution.
IICPC_FORCE_INLINE IICPC_AVX2_TARGET
void avx2_zero_cachelines(void* dst, std::size_t num_cachelines) noexcept {
    auto* d = static_cast<__m256i*>(dst);
    const __m256i vzero = _mm256_setzero_si256();
    for (std::size_t i = 0; i < num_cachelines * 2; ++i) {
        _mm256_stream_si256(d + i, vzero);
    }
    _mm_sfence(); // Ensure streaming stores are globally visible
}

// =============================================================================
// AVX-512 SIMD Helpers — 512-bit (64 bytes = 1 cache line!) operations
// =============================================================================
// Available on Ice Lake / Sapphire Rapids server CPUs and Alder Lake P-cores
// (with AVX-512 enabled in BIOS — disabled by default on consumer Alder Lake).
//
// Guard with __AVX512F__ so builds work on CPUs without AVX-512.
// =============================================================================

#ifdef __AVX512F__

/// Compare 8 × int64 keys against search key. Returns 8-bit mask.
/// Processes an entire cache line of int64 keys in ONE instruction.
IICPC_FORCE_INLINE
uint8_t avx512_find_key_i64(const int64_t* keys, int64_t search_key) noexcept {
    __m512i vkeys = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(keys));
    __m512i vtarget = _mm512_set1_epi64(search_key);
    return _mm512_cmpeq_epi64_mask(vkeys, vtarget);
}

/// Check if any of 8 × int64 keys match.
IICPC_FORCE_INLINE
bool avx512_contains_key_i64(const int64_t* keys, int64_t search_key) noexcept {
    return avx512_find_key_i64(keys, search_key) != 0;
}

/// Extract first match index from AVX-512 8-bit mask.
IICPC_FORCE_INLINE
uint32_t avx512_first_match_index_i64(uint8_t mask) noexcept {
    return static_cast<uint32_t>(__builtin_ctz(static_cast<unsigned>(mask)));
}

/// Compare 16 × int32 keys against search key. Returns 16-bit mask.
IICPC_FORCE_INLINE
uint16_t avx512_find_key_i32(const int32_t* keys, int32_t search_key) noexcept {
    __m512i vkeys = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(keys));
    __m512i vtarget = _mm512_set1_epi32(search_key);
    return _mm512_cmpeq_epi32_mask(vkeys, vtarget);
}

/// Vectorized cache-line copy using single AVX-512 load/store.
/// Copies exactly 64 bytes (1 cache line) in 2 instructions.
IICPC_FORCE_INLINE
void avx512_copy_cacheline(void* IICPC_RESTRICT dst,
                            const void* IICPC_RESTRICT src) noexcept {
    __m512i data = _mm512_loadu_si512(static_cast<const __m512i*>(src));
    _mm512_storeu_si512(static_cast<__m512i*>(dst), data);
}

/// AVX-512 streaming (non-temporal) cache-line copy.
IICPC_FORCE_INLINE
void avx512_stream_cacheline(void* IICPC_RESTRICT dst,
                              const void* IICPC_RESTRICT src) noexcept {
    __m512i data = _mm512_loadu_si512(static_cast<const __m512i*>(src));
    _mm512_stream_si512(static_cast<__m512i*>(dst), data);
}

/// AVX-512 batch search: find price in SoA array, 8 elements per iteration.
IICPC_FORCE_INLINE
uint32_t avx512_linear_search_i64(const int64_t* arr, std::size_t count,
                                   int64_t target) noexcept {
    const __m512i vtarget = _mm512_set1_epi64(target);
    std::size_t i = 0;

    for (; i + 8 <= count; i += 8) {
        __m512i vdata = _mm512_loadu_si512(
            reinterpret_cast<const __m512i*>(arr + i));
        uint8_t mask = _mm512_cmpeq_epi64_mask(vdata, vtarget);
        if (mask != 0) {
            return static_cast<uint32_t>(i) +
                   static_cast<uint32_t>(__builtin_ctz(static_cast<unsigned>(mask)));
        }
    }

    // Scalar tail
    for (; i < count; ++i) {
        if (arr[i] == target) return static_cast<uint32_t>(i);
    }
    return UINT32_MAX;
}

/// AVX-512 zero: zero out N cache lines using streaming stores.
IICPC_FORCE_INLINE
void avx512_zero_cachelines(void* dst, std::size_t num_cachelines) noexcept {
    auto* d = static_cast<__m512i*>(dst);
    const __m512i vzero = _mm512_setzero_si512();
    for (std::size_t i = 0; i < num_cachelines; ++i) {
        _mm512_stream_si512(d + i, vzero);
    }
    _mm_sfence();
}

#endif // __AVX512F__

// =============================================================================
// Portable SIMD dispatch — automatically uses best available ISA
// =============================================================================

/// Find int64 key in array: AVX-512 → AVX2 → scalar fallback
IICPC_FORCE_INLINE IICPC_AVX2_TARGET
uint32_t simd_find_key_i64(const int64_t* keys, std::size_t count,
                            int64_t target) noexcept {
#ifdef __AVX512F__
    return avx512_linear_search_i64(keys, count, target);
#else
    return avx2_linear_search_i64(keys, count, target);
#endif
}

/// Copy one cache line: AVX-512 → AVX2 dispatch
IICPC_FORCE_INLINE IICPC_AVX2_TARGET
void simd_copy_cacheline(void* IICPC_RESTRICT dst,
                          const void* IICPC_RESTRICT src) noexcept {
#ifdef __AVX512F__
    avx512_copy_cacheline(dst, src);
#else
    avx2_copy_cacheline(dst, src);
#endif
}

/// Stream one cache line: AVX-512 → AVX2 dispatch
IICPC_FORCE_INLINE IICPC_AVX2_TARGET
void simd_stream_cacheline(void* IICPC_RESTRICT dst,
                            const void* IICPC_RESTRICT src) noexcept {
#ifdef __AVX512F__
    avx512_stream_cacheline(dst, src);
#else
    avx2_stream_cacheline(dst, src);
#endif
}

/// Zero N cache lines: AVX-512 → AVX2 dispatch
IICPC_FORCE_INLINE IICPC_AVX2_TARGET
void simd_zero_cachelines(void* dst, std::size_t num_cachelines) noexcept {
#ifdef __AVX512F__
    avx512_zero_cachelines(dst, num_cachelines);
#else
    avx2_zero_cachelines(dst, num_cachelines);
#endif
}

} // namespace iicpc
