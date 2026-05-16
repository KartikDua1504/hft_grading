// =============================================================================
// bench_shm.cpp — Shared Memory Benchmark (Zero Syscall Hot Path)
// =============================================================================
// Pure userspace IPC: loadgen and exchange communicate via lock-free SPSC
// ring buffers in shared memory. NO sockets, NO kernel, NO syscalls.
//
// This is the absolute performance ceiling for this architecture.
// Expected: <1µs p50 (cache-line transfer latency only)
// =============================================================================

#include "core/arena.hpp"
#include "core/ring_buffer.hpp"
#include "core/tsc.hpp"
#include "core/hdr_histogram.hpp"
#include "core/types.hpp"
#include "core/compiler_hints.hpp"
#include "loadgen/bot_fleet.hpp"
#include "loadgen/payload_gen.hpp"
#include "loadgen/shm_engine.hpp"
#include "telemetry/consumer.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <sched.h>
#include <thread>
#include <unistd.h>

using namespace iicpc;

static volatile sig_atomic_t g_running = 1;
static void signal_handler(int) { g_running = 0; }

static constexpr std::size_t CHANNEL_CAPACITY = 65536; // 64K entries

struct ShmBenchConfig {
    int duration_secs = 10;
    std::size_t num_bots = 8;
    int exchange_cpu = -1;
    int loadgen_cpu = -1;
    int telemetry_cpu = -1;
};

static ShmBenchConfig parse_args(int argc, char* argv[]) {
    ShmBenchConfig cfg;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            cfg.duration_secs = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--bots") == 0 && i + 1 < argc)
            cfg.num_bots = static_cast<std::size_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--exchange-cpu") == 0 && i + 1 < argc)
            cfg.exchange_cpu = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--loadgen-cpu") == 0 && i + 1 < argc)
            cfg.loadgen_cpu = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--telemetry-cpu") == 0 && i + 1 < argc)
            cfg.telemetry_cpu = std::atoi(argv[++i]);
    }
    return cfg;
}

int main(int argc, char* argv[]) {
    auto cfg = parse_args(argc, argv);

    ::signal(SIGINT, signal_handler);
    ::signal(SIGTERM, signal_handler);
    ::signal(SIGPIPE, SIG_IGN);

    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "╔══════════════════════════════════════════════════════════╗\n");
    std::fprintf(stderr, "║   SHARED MEMORY BENCHMARK (ZERO SYSCALLS)               ║\n");
    std::fprintf(stderr, "║   Pure lock-free IPC: loadgen ↔ exchange                ║\n");
    std::fprintf(stderr, "║   Target: <1µs p50, maximum TPS                         ║\n");
    std::fprintf(stderr, "╚══════════════════════════════════════════════════════════╝\n\n");

    // Arena
    HugePageArena arena;
    (void)arena.init(1024ULL * 1024 * 1024);
    auto tsc_cal = calibrate_tsc();

    // Telemetry ring
    using TelemetryRing = SPSCRingBuffer<LatencySample, 1048576>;
    void* ring_mem = arena.allocate_raw(TelemetryRing::TOTAL_SIZE, CACHE_LINE_SIZE);
    TelemetryRing ring(ring_mem, true);

    // Shared-memory channels (allocated from hugepage arena)
    using ReqChannel  = ShmChannel<ShmRequest, CHANNEL_CAPACITY>;
    using RespChannel = ShmChannel<ShmResponse, CHANNEL_CAPACITY>;

    auto* req_buf = static_cast<ShmRequest*>(
        arena.allocate_raw(sizeof(ShmRequest) * CHANNEL_CAPACITY, CACHE_LINE_SIZE));
    auto* resp_buf = static_cast<ShmResponse*>(
        arena.allocate_raw(sizeof(ShmResponse) * CHANNEL_CAPACITY, CACHE_LINE_SIZE));

    ReqChannel req_chan;
    RespChannel resp_chan;
    req_chan.init(req_buf);
    resp_chan.init(resp_buf);

    // Exchange thread
    ShmExchange<CHANNEL_CAPACITY> exchange;
    exchange.init(req_chan, resp_chan);
    std::atomic<bool> exchange_stop{false};

    std::thread exchange_thread([&]() {
        if (cfg.exchange_cpu >= 0) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(cfg.exchange_cpu, &cpuset);
            ::sched_setaffinity(0, sizeof(cpuset), &cpuset);
            std::fprintf(stderr, "[exchange] Pinned to CPU %d\n", cfg.exchange_cpu);
        }

        // Busy-loop: process requests as fast as possible
        while (!exchange_stop.load(std::memory_order_acquire)) {
            if (exchange.process_batch() == 0) {
                cpu_pause(); // Yield hint when idle
            }
        }
        // Drain remaining
        exchange.process_batch();
    });

    // Bot fleet
    BotFleet fleet;
    fleet.init(arena, cfg.num_bots);

    // SHM engine
    ShmEngine<CHANNEL_CAPACITY> engine;
    engine.init(req_chan, resp_chan);

    // Telemetry consumer
    TelemetryConsumer consumer;
    ConsumerConfig consumer_config{};
    consumer_config.cpu_affinity = cfg.telemetry_cpu;
    consumer_config.report_interval_ms = 1000;
    consumer.init(consumer_config, tsc_cal);

    std::thread consumer_thread([&]() { consumer.run(ring); });

    // Benchmark
    std::fprintf(stderr, "[bench] %zu bots, channel capacity %zu\n",
                 cfg.num_bots, CHANNEL_CAPACITY);
    std::fprintf(stderr, "\n[bench] === SHM BENCHMARK START (%d seconds) ===\n\n",
                 cfg.duration_secs);

    if (cfg.loadgen_cpu >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cfg.loadgen_cpu, &cpuset);
        ::sched_setaffinity(0, sizeof(cpuset), &cpuset);
        std::fprintf(stderr, "[bench] Loadgen pinned to CPU %d\n", cfg.loadgen_cpu);
    }

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(cfg.duration_secs);

    while (g_running && std::chrono::steady_clock::now() < deadline) {
        engine.run_batch(fleet, ring);
    }

    // Drain
    const auto end = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(end - start).count();

    const auto drain_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < drain_end) {
        for (std::size_t i = 0; i < fleet.count; ++i)
            if (fleet.states[i] == BotState::IDLE)
                fleet.states[i] = BotState::RECEIVED;
        engine.run_batch(fleet, ring);
    }

    consumer.stop();
    if (consumer_thread.joinable()) consumer_thread.join();
    exchange_stop.store(true);
    if (exchange_thread.joinable()) exchange_thread.join();

    // Results
    const uint64_t sends = engine.total_sends();
    const uint64_t recvs = engine.total_recvs();
    const double tps = static_cast<double>(recvs) / elapsed;
    const uint64_t drops = (sends > recvs) ? sends - recvs : 0;
    auto pct = consumer.latest_percentiles();

    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "╔══════════════════════════════════════════════════════════════╗\n");
    std::fprintf(stderr, "║            SHARED MEMORY BENCHMARK RESULTS                  ║\n");
    std::fprintf(stderr, "╠══════════════════════════════════════════════════════════════╣\n");
    std::fprintf(stderr, "║  Duration:     %.2f seconds                                ║\n", elapsed);
    std::fprintf(stderr, "║  Transport:    Shared Memory (zero syscalls)                 ║\n");
    std::fprintf(stderr, "║  Bots:         %-44zu  ║\n", cfg.num_bots);
    std::fprintf(stderr, "║  Channel Cap:  %-44zu  ║\n", CHANNEL_CAPACITY);
    std::fprintf(stderr, "║                                                              ║\n");
    std::fprintf(stderr, "║  Total Sends:  %-44lu  ║\n", sends);
    std::fprintf(stderr, "║  Total Recvs:  %-44lu  ║\n", recvs);
    std::fprintf(stderr, "║  Drops:        %-44lu  ║\n", drops);
    std::fprintf(stderr, "║  TPS:          %-44.0f  ║\n", tps);
    std::fprintf(stderr, "║                                                              ║\n");
    std::fprintf(stderr, "║  Latency:                                                    ║\n");
    std::fprintf(stderr, "║    min:   %10.2f µs                                       ║\n",
                 static_cast<double>(pct.min) / 1000.0);
    std::fprintf(stderr, "║    p50:   %10.2f µs                                       ║\n",
                 static_cast<double>(pct.p50) / 1000.0);
    std::fprintf(stderr, "║    p90:   %10.2f µs                                       ║\n",
                 static_cast<double>(pct.p90) / 1000.0);
    std::fprintf(stderr, "║    p99:   %10.2f µs                                       ║\n",
                 static_cast<double>(pct.p99) / 1000.0);
    std::fprintf(stderr, "║    p999:  %10.2f µs                                       ║\n",
                 static_cast<double>(pct.p999) / 1000.0);
    std::fprintf(stderr, "║    max:   %10.2f µs                                       ║\n",
                 static_cast<double>(pct.max) / 1000.0);
    std::fprintf(stderr, "║                                                              ║\n");

    const bool tps_1m = tps >= 1'000'000.0;
    const bool tps_5m = tps >= 5'000'000.0;
    const bool tps_10m = tps >= 10'000'000.0;
    const bool drops_ok = drops == 0;
    const bool p50_1us = pct.p50 <= 1000;
    const bool p50_5us = pct.p50 <= 5000;
    const bool p99_10us = pct.p99 <= 10000;
    const bool p999_50us = pct.p999 <= 50000;

    std::fprintf(stderr, "║  [%s] TPS ≥ 1M                                             ║\n",
                 tps_1m ? "✓" : "✗");
    std::fprintf(stderr, "║  [%s] TPS ≥ 5M                                             ║\n",
                 tps_5m ? "✓" : "✗");
    std::fprintf(stderr, "║  [%s] TPS ≥ 10M                                            ║\n",
                 tps_10m ? "✓" : "✗");
    std::fprintf(stderr, "║  [%s] Zero drops                                           ║\n",
                 drops_ok ? "✓" : "✗");
    std::fprintf(stderr, "║  [%s] p50 < 1µs (physics limit)                            ║\n",
                 p50_1us ? "✓" : "✗");
    std::fprintf(stderr, "║  [%s] p50 < 5µs (HFT-tier)                                ║\n",
                 p50_5us ? "✓" : "✗");
    std::fprintf(stderr, "║  [%s] p99 < 10µs (ultra-deterministic)                     ║\n",
                 p99_10us ? "✓" : "✗");
    std::fprintf(stderr, "║  [%s] p999 < 50µs                                          ║\n",
                 p999_50us ? "✓" : "✗");
    std::fprintf(stderr, "╚══════════════════════════════════════════════════════════════╝\n");

    return 0;
}
