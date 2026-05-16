// =============================================================================
// bench_arena.cpp — Arena Allocator Micro-Benchmark
// =============================================================================
#include "core/arena.hpp"
#include "core/tsc.hpp"
#include "core/types.hpp"
#include <cstdio>
#include <cstdint>

using namespace iicpc;

int main() {
    std::fprintf(stderr, "=== Arena Allocator Benchmark ===\n\n");

    auto tsc_cal = calibrate_tsc();

    // Test 1: Hugepage initialization
    HugePageArena arena;
    bool hugepages = arena.init(256 * 1024 * 1024); // 256 MiB
    std::fprintf(stderr, "[test] Hugepage backed: %s\n", hugepages ? "YES" : "NO (fallback)");
    std::fprintf(stderr, "[test] Arena capacity: %zu MiB\n", arena.capacity() / (1024 * 1024));

    // Test 2: Alignment verification
    bool all_aligned = true;
    for (int i = 0; i < 1000; ++i) {
        void* ptr = arena.allocate_raw(1 + (i % 128));
        if (reinterpret_cast<uintptr_t>(ptr) % CACHE_LINE_SIZE != 0) {
            std::fprintf(stderr, "[FAIL] Allocation %d not 64-byte aligned: %p\n", i, ptr);
            all_aligned = false;
        }
    }
    std::fprintf(stderr, "[test] All 1000 allocations 64-byte aligned: %s\n",
                 all_aligned ? "PASS" : "FAIL");

    // Test 3: Allocation throughput
    arena.reset();
    constexpr int NUM_ALLOCS = 1'000'000;
    uint64_t start = rdtscp();
    for (int i = 0; i < NUM_ALLOCS; ++i) {
        volatile void* ptr = arena.allocate_raw(64);
        (void)ptr;
    }
    uint64_t end = rdtscp();
    double ns_per_alloc = tsc_to_ns(end - start, tsc_cal) / NUM_ALLOCS;
    std::fprintf(stderr, "[test] Allocation throughput: %.1f ns/alloc (%d allocations)\n",
                 ns_per_alloc, NUM_ALLOCS);
    std::fprintf(stderr, "[test] Arena used: %zu / %zu bytes (%.1f%%)\n",
                 arena.used(), arena.capacity(),
                 100.0 * static_cast<double>(arena.used()) / static_cast<double>(arena.capacity()));

    // Test 4: Reset speed
    start = rdtscp();
    for (int i = 0; i < 1'000'000; ++i) {
        arena.reset();
    }
    end = rdtscp();
    double ns_per_reset = tsc_to_ns(end - start, tsc_cal) / 1'000'000;
    std::fprintf(stderr, "[test] Reset speed: %.1f ns/reset\n", ns_per_reset);

    std::fprintf(stderr, "\n=== Arena Benchmark Complete ===\n");
    return all_aligned ? 0 : 1;
}
