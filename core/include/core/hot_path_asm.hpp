#pragma once
// =============================================================================
// hot_path_asm.hpp — Inline Assembly for Critical Hot Path Operations
// =============================================================================
// Hand-tuned x86-64 assembly for operations where compiler codegen matters.
// Every nanosecond counts in the measurement loop.
//
// Why not just use intrinsics?
//   - rdtsc: __rdtsc() adds unnecessary compiler barriers
//   - memcpy: compiler generates rep movsb, we want explicit movdqa for 64B
//   - pause: _mm_pause() is fine but we want tighter spin control
//   - lfence: compiler may reorder around intrinsic calls
//
// All functions are FORCE_INLINE — zero call overhead.
// =============================================================================

#include <cstdint>
#include <cstddef>

namespace iicpc {
namespace asm_hot {

// =============================================================================
// RDTSC — Read Time Stamp Counter (serialized)
// =============================================================================
// lfence; rdtsc — serialized read. lfence prevents out-of-order execution
// from moving rdtsc ahead of prior instructions. This is the gold standard
// for latency measurement on Intel/AMD.
//
// Cost: ~25 cycles (lfence) + ~20 cycles (rdtsc) = ~45 cycles total
// Compare: __rdtsc() alone is ~20 cycles but NOT serialized
// =============================================================================
[[nodiscard]] __attribute__((always_inline))
inline uint64_t rdtsc_serialized() noexcept {
    uint32_t lo, hi;
    asm volatile(
        "lfence\n\t"       // Serialize: complete all prior instructions
        "rdtsc\n\t"        // Read TSC into edx:eax
        : "=a"(lo), "=d"(hi)
        :
        : "memory"         // Compiler barrier
    );
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

// =============================================================================
// RDTSCP — Read TSC + Processor ID (already serialized on read side)
// =============================================================================
// rdtscp waits for all prior instructions to complete before reading.
// Also returns processor ID in ecx (useful for detecting core migration).
// Followed by lfence to prevent subsequent instructions from moving ahead.
// =============================================================================
[[nodiscard]] __attribute__((always_inline))
inline uint64_t rdtscp_end(uint32_t* cpu_id = nullptr) noexcept {
    uint32_t lo, hi, aux;
    asm volatile(
        "rdtscp\n\t"       // Serialized TSC read + cpu_id in ecx
        "lfence\n\t"       // Prevent later instructions from reordering
        : "=a"(lo), "=d"(hi), "=c"(aux)
        :
        : "memory"
    );
    if (cpu_id) *cpu_id = aux;
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

// =============================================================================
// Precision Latency Measurement Pair
// =============================================================================
// Usage:
//   uint64_t start = asm_hot::timestamp_begin();
//   ... work ...
//   uint64_t end = asm_hot::timestamp_end();
//   uint64_t cycles = end - start;
// =============================================================================
[[nodiscard]] __attribute__((always_inline))
inline uint64_t timestamp_begin() noexcept {
    uint32_t lo, hi;
    asm volatile(
        "cpuid\n\t"        // Full serialization (heavier than lfence)
        "rdtsc\n\t"
        : "=a"(lo), "=d"(hi)
        : "a"(0)           // cpuid leaf 0
        : "rbx", "rcx", "memory"
    );
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

[[nodiscard]] __attribute__((always_inline))
inline uint64_t timestamp_end() noexcept {
    return rdtscp_end();
}

// =============================================================================
// Cache-Line Copy (64 bytes) — Single movdqa pair
// =============================================================================
// For copying cache-line-sized protocol messages (OrderEntry, Fill, etc.)
// Uses SSE2 aligned loads/stores. 4x movdqa = 64 bytes in 4 cycles.
// Compiler's memcpy may use rep movsb which has startup overhead.
// =============================================================================
__attribute__((always_inline))
inline void copy_cacheline(void* __restrict__ dst,
                           const void* __restrict__ src) noexcept {
    asm volatile(
        "movdqa   (%[src]),    %%xmm0\n\t"    // Load 16B [0:15]
        "movdqa 16(%[src]),    %%xmm1\n\t"    // Load 16B [16:31]
        "movdqa 32(%[src]),    %%xmm2\n\t"    // Load 16B [32:47]
        "movdqa 48(%[src]),    %%xmm3\n\t"    // Load 16B [48:63]
        "movdqa %%xmm0,   (%[dst])\n\t"      // Store 16B [0:15]
        "movdqa %%xmm1, 16(%[dst])\n\t"      // Store 16B [16:31]
        "movdqa %%xmm2, 32(%[dst])\n\t"      // Store 16B [32:47]
        "movdqa %%xmm3, 48(%[dst])\n\t"      // Store 16B [48:63]
        :
        : [dst] "r"(dst), [src] "r"(src)
        : "xmm0", "xmm1", "xmm2", "xmm3", "memory"
    );
}

// =============================================================================
// Half Cache-Line Copy (32 bytes) — For OrderAck, CancelAck, Heartbeat
// =============================================================================
__attribute__((always_inline))
inline void copy_half_cacheline(void* __restrict__ dst,
                                const void* __restrict__ src) noexcept {
    asm volatile(
        "movdqa   (%[src]),    %%xmm0\n\t"
        "movdqa 16(%[src]),    %%xmm1\n\t"
        "movdqa %%xmm0,   (%[dst])\n\t"
        "movdqa %%xmm1, 16(%[dst])\n\t"
        :
        : [dst] "r"(dst), [src] "r"(src)
        : "xmm0", "xmm1", "memory"
    );
}

// =============================================================================
// Spin-Wait with Exponential Backoff (assembly PAUSE loop)
// =============================================================================
// PAUSE instruction: ~140 cycles on Alder Lake (was ~10 on older CPUs).
// Reduces power consumption and memory bus contention during spin-waits.
// Exponential backoff prevents livelock under high contention.
// =============================================================================
__attribute__((always_inline))
inline void spin_pause_n(uint32_t count) noexcept {
    asm volatile(
        "1:\n\t"
        "pause\n\t"
        "sub $1, %[cnt]\n\t"
        "jnz 1b\n\t"
        : [cnt] "+r"(count)
        :
        :
    );
}

// =============================================================================
// Non-Temporal Store (bypass cache, write-combine buffer)
// =============================================================================
// For telemetry writes that won't be re-read soon. Avoids polluting L1/L2.
// Uses movntdq (non-temporal store) + sfence after batch.
// =============================================================================
__attribute__((always_inline))
inline void store_cacheline_nt(void* dst, const void* src) noexcept {
    asm volatile(
        "movdqa   (%[src]),    %%xmm0\n\t"
        "movdqa 16(%[src]),    %%xmm1\n\t"
        "movdqa 32(%[src]),    %%xmm2\n\t"
        "movdqa 48(%[src]),    %%xmm3\n\t"
        "movntdq %%xmm0,   (%[dst])\n\t"     // Non-temporal store
        "movntdq %%xmm1, 16(%[dst])\n\t"
        "movntdq %%xmm2, 32(%[dst])\n\t"
        "movntdq %%xmm3, 48(%[dst])\n\t"
        :
        : [dst] "r"(dst), [src] "r"(src)
        : "xmm0", "xmm1", "xmm2", "xmm3", "memory"
    );
}

/// Must call after a batch of non-temporal stores
__attribute__((always_inline))
inline void nt_store_fence() noexcept {
    asm volatile("sfence" ::: "memory");
}

// =============================================================================
// Prefetch Chain (for SoA array traversal)
// =============================================================================
__attribute__((always_inline))
inline void prefetch_l1_read(const void* addr) noexcept {
    asm volatile("prefetcht0 (%[addr])" : : [addr] "r"(addr));
}

__attribute__((always_inline))
inline void prefetch_l2_read(const void* addr) noexcept {
    asm volatile("prefetcht1 (%[addr])" : : [addr] "r"(addr));
}

__attribute__((always_inline))
inline void prefetch_nta(const void* addr) noexcept {
    asm volatile("prefetchnta (%[addr])" : : [addr] "r"(addr));
}

} // namespace asm_hot
} // namespace iicpc
