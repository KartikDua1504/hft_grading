// --- io_uring vs epoll Transport Engine Benchmark ---
// Compares throughput and latency of both transport engines
// against the dummy exchange server.
//
// Usage: Start dummy_exchange first, then run this benchmark.
//   ./dummy_exchange &
//   ./bench_iouring

#include "loadgen/io_engine.hpp"
#include "loadgen/bot_fleet.hpp"
#include "loadgen/payload_gen.hpp"
#include "core/arena.hpp"
#include "core/tsc.hpp"
#include "core/hdr_histogram.hpp"
#include "core/ring_buffer.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace iicpc;

// --- Benchmark Configuration ---
static constexpr std::size_t NUM_BOTS       = 32;
static constexpr std::size_t BATCH_SIZE     = 128;
static constexpr double      DURATION_SECS  = 5.0;
static constexpr uint16_t    TARGET_PORT    = 9999;

struct BenchResult {
    const char* engine_name = nullptr;
    uint64_t    total_sends = 0;
    uint64_t    total_recvs = 0;
    double      duration_secs = 0.0;
    double      tps = 0.0;
    double      p50_us = 0.0;
    double      p90_us = 0.0;
    double      p99_us = 0.0;
    double      p999_us = 0.0;
    double      max_us = 0.0;
};

static BenchResult run_engine_bench(
    const char* label,
    IoEngine* engine,
    HugePageArena& arena,
    const TscCalibration& tsc_cal) noexcept {

    BenchResult result{};
    result.engine_name = label;

    // Init fleet
    BotFleet fleet{};
    if (!fleet.init(arena, NUM_BOTS)) {
        std::fprintf(stderr, "[%s] Failed to init fleet\n", label);
        return result;
    }

    // Generate payloads
    if (!generate_payloads(arena, fleet)) {
        std::fprintf(stderr, "[%s] Failed to generate payloads\n", label);
        return result;
    }

    // Init engine
    IoEngineConfig config{};
    config.target_host = "127.0.0.1";
    config.target_port = TARGET_PORT;
    config.batch_size  = BATCH_SIZE;
    config.ring_depth  = 4096;
    config.sqpoll      = true;

    if (!engine->init(config, fleet, arena)) {
        std::fprintf(stderr, "[%s] Failed to init engine — is dummy_exchange running on port %u?\n",
                     label, TARGET_PORT);
        return result;
    }

    // Telemetry ring
    using TelemetryRing = SPSCRingBuffer<LatencySample, 1048576>;
    void* ring_mem = arena.allocate_raw(TelemetryRing::TOTAL_SIZE, CACHE_LINE_SIZE);
    if (!ring_mem) {
        std::fprintf(stderr, "[%s] Failed to allocate telemetry ring\n", label);
        return result;
    }
    TelemetryRing telemetry_ring(ring_mem, true);

    // HDR histogram for latency
    HdrHistogram hdr{};
    hdr.init();

    std::fprintf(stderr, "\n[%s] Running for %.1f seconds with %zu bots...\n",
                 label, DURATION_SECS, NUM_BOTS);

    // --- Main benchmark loop ---
    auto start = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::duration<double>(DURATION_SECS);

    while (std::chrono::steady_clock::now() < deadline) {
        engine->run_batch(fleet, telemetry_ring);

        // Drain telemetry samples into histogram
        LatencySample sample{};
        while (telemetry_ring.try_pop(sample)) {
            if (sample.recv_tsc > sample.send_tsc) {
                uint64_t ticks = sample.recv_tsc - sample.send_tsc;
                double ns = tsc_to_ns(ticks, tsc_cal);
                hdr.record(static_cast<uint64_t>(ns));
            }
        }
    }

    auto end = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(end - start).count();

    engine->shutdown();

    // Close bot sockets (epoll engine doesn't close them in shutdown)
    for (std::size_t i = 0; i < fleet.count; ++i) {
        if (fleet.socket_fds[i] >= 0) {
            ::close(fleet.socket_fds[i]);
            fleet.socket_fds[i] = -1;
        }
    }

    result.total_sends   = engine->total_sends();
    result.total_recvs   = engine->total_recvs();
    result.duration_secs = elapsed;
    result.tps           = static_cast<double>(result.total_recvs) / elapsed;

    auto pcts = hdr.get_percentiles();
    result.p50_us  = static_cast<double>(pcts.p50) / 1000.0;
    result.p90_us  = static_cast<double>(pcts.p90) / 1000.0;
    result.p99_us  = static_cast<double>(pcts.p99) / 1000.0;
    result.p999_us = static_cast<double>(pcts.p999) / 1000.0;
    result.max_us  = static_cast<double>(pcts.max) / 1000.0;

    return result;
}

static void print_result(const BenchResult& r) noexcept {
    std::fprintf(stderr, "\n--- %s Results ---\n", r.engine_name);
    std::fprintf(stderr, "  Duration:    %.2f s\n", r.duration_secs);
    std::fprintf(stderr, "  Sends:       %lu\n", r.total_sends);
    std::fprintf(stderr, "  Recvs:       %lu\n", r.total_recvs);
    std::fprintf(stderr, "  Throughput:  %.2f K TPS\n", r.tps / 1000.0);
    std::fprintf(stderr, "  Latency p50: %.1f us\n", r.p50_us);
    std::fprintf(stderr, "  Latency p90: %.1f us\n", r.p90_us);
    std::fprintf(stderr, "  Latency p99: %.1f us\n", r.p99_us);
    std::fprintf(stderr, "  Latency p999:%.1f us\n", r.p999_us);
    std::fprintf(stderr, "  Latency max: %.1f us\n", r.max_us);
}

int main() {
    std::fprintf(stderr, "--- io_uring vs epoll Transport Benchmark ---\n");
    std::fprintf(stderr, "  Bots: %zu | Batch: %zu | Duration: %.0fs per engine\n",
                 NUM_BOTS, BATCH_SIZE, DURATION_SECS);
    std::fprintf(stderr, "  Ensure dummy_exchange is running on port %u\n\n", TARGET_PORT);

    // Calibrate TSC
    auto tsc_cal = calibrate_tsc();
    std::fprintf(stderr, "[tsc] Calibrated: %.3f GHz (%.3f ns/tick)\n",
                 tsc_cal.tsc_ghz, tsc_cal.tsc_to_ns);

    // --- Run epoll benchmark ---
    {
        HugePageArena arena{};
        if (!arena.init(64 * 1024 * 1024)) { // 64 MB
            std::fprintf(stderr, "Failed to init arena for epoll bench\n");
            return 1;
        }

        EpollIoEngine epoll_engine{};
        BenchResult epoll_result = run_engine_bench("epoll", &epoll_engine, arena, tsc_cal);
        print_result(epoll_result);
        arena.reset();
    }

    // Brief pause between benchmarks
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // --- Run io_uring benchmark ---
#if IICPC_HAS_URING
    {
        HugePageArena arena{};
        if (!arena.init(64 * 1024 * 1024)) { // 64 MB
            std::fprintf(stderr, "Failed to init arena for io_uring bench\n");
            return 1;
        }

        IoUringEngine uring_engine{};
        BenchResult uring_result = run_engine_bench(
            uring_engine.name(), &uring_engine, arena, tsc_cal);
        print_result(uring_result);
        arena.reset();
    }
#else
    std::fprintf(stderr, "\n[SKIP] io_uring not available (IICPC_HAS_URING=0)\n");
#endif

    std::fprintf(stderr, "\n--- Benchmark Complete ---\n");
    return 0;
}
