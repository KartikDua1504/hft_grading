// =============================================================================
// bench_ringbuf.cpp — SPSC Ring Buffer Micro-Benchmark
// =============================================================================
#include "core/ring_buffer.hpp"
#include "core/tsc.hpp"
#include "core/types.hpp"
#include <cstdio>
#include <thread>

using namespace iicpc;

int main() {
    std::fprintf(stderr, "=== SPSC Ring Buffer Benchmark ===\n\n");

    auto tsc_cal = calibrate_tsc();

    // Allocate ring buffer memory
    constexpr std::size_t RING_CAP = 1048576; // 1M slots
    using Ring = SPSCRingBuffer<LatencySample, RING_CAP>;

    // Use a simple aligned allocation for the benchmark
    void* mem = std::aligned_alloc(CACHE_LINE_SIZE, Ring::TOTAL_SIZE);
    if (!mem) {
        std::fprintf(stderr, "FATAL: Failed to allocate ring buffer memory\n");
        return 1;
    }

    Ring ring(mem, true);

    // Test 1: Cache line separation
    bool cache_ok = Ring::verify_cache_line_separation(mem);
    std::fprintf(stderr, "[test] Cache line separation: %s\n", cache_ok ? "PASS" : "FAIL");

    // Print actual offsets
    const auto* hdr = static_cast<const RingBufferHeader*>(mem);
    std::fprintf(stderr, "[test] write_pos addr: %p (offset: %zu)\n",
                 static_cast<const void*>(&hdr->write_pos),
                 reinterpret_cast<uintptr_t>(&hdr->write_pos) - reinterpret_cast<uintptr_t>(hdr));
    std::fprintf(stderr, "[test] read_pos addr:  %p (offset: %zu)\n",
                 static_cast<const void*>(&hdr->read_pos),
                 reinterpret_cast<uintptr_t>(&hdr->read_pos) - reinterpret_cast<uintptr_t>(hdr));

    // Test 2: Single-threaded push/pop throughput
    constexpr uint64_t OPS = 10'000'000;
    LatencySample sample{.send_tsc = 1, .recv_tsc = 2, .bot_id = 0, .seq_num = 0};

    uint64_t start = rdtscp();
    for (uint64_t i = 0; i < OPS; ++i) {
        sample.seq_num = static_cast<uint32_t>(i);
        (void)ring.try_push(sample);
        (void)ring.try_pop(sample);
    }
    uint64_t end = rdtscp();
    double ns_per_op = tsc_to_ns(end - start, tsc_cal) / (OPS * 2);
    std::fprintf(stderr, "[test] Single-threaded push+pop: %.1f ns/op (%.1fM ops/sec)\n",
                 ns_per_op, 1000.0 / ns_per_op);

    // Test 3: Two-threaded SPSC throughput
    Ring ring2(mem, true); // Re-init

    std::atomic<bool> producer_done{false};
    uint64_t consumer_count = 0;

    uint64_t t_start = rdtscp();

    std::thread producer([&]() {
        LatencySample s{.send_tsc = 1, .recv_tsc = 2, .bot_id = 0, .seq_num = 0};
        for (uint64_t i = 0; i < OPS; ++i) {
            s.seq_num = static_cast<uint32_t>(i);
            while (!ring2.try_push(s)) {
                _mm_pause();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&]() {
        LatencySample s{};
        while (!producer_done.load(std::memory_order_acquire) || !ring2.empty()) {
            if (ring2.try_pop(s)) {
                consumer_count++;
            } else {
                _mm_pause();
            }
        }
    });

    producer.join();
    consumer.join();

    uint64_t t_end = rdtscp();
    double elapsed_ns = tsc_to_ns(t_end - t_start, tsc_cal);
    double mops = static_cast<double>(consumer_count) / (elapsed_ns / 1000.0);

    std::fprintf(stderr, "[test] Two-threaded SPSC: %lu ops in %.1f ms (%.1fM ops/sec)\n",
                 consumer_count, elapsed_ns / 1e6, mops);
    std::fprintf(stderr, "[test] All messages received: %s (%lu / %lu)\n",
                 consumer_count == OPS ? "PASS" : "FAIL", consumer_count, OPS);

    std::free(mem);
    std::fprintf(stderr, "\n=== Ring Buffer Benchmark Complete ===\n");
    return (cache_ok && consumer_count == OPS) ? 0 : 1;
}
