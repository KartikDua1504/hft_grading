// =============================================================================
// bench_pipeline.cpp — Pipeline Multiplexed Benchmark
// =============================================================================
// Tests the PipelineEngine which uses writev() scatter-gather I/O
// to batch N bot messages through K sockets in K syscalls instead of N.
//
// This is the next level beyond the ultra engine.
// =============================================================================

#include "core/arena.hpp"
#include "core/ring_buffer.hpp"
#include "core/tsc.hpp"
#include "core/hdr_histogram.hpp"
#include "core/types.hpp"
#include "core/compiler_hints.hpp"
#include "loadgen/bot_fleet.hpp"
#include "loadgen/payload_gen.hpp"
#include "loadgen/ultra_engine.hpp"   // For UltraExchange
#include "loadgen/pipeline_engine.hpp"
#include "telemetry/consumer.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <thread>
#include <unistd.h>

using namespace iicpc;

static volatile sig_atomic_t g_running = 1;
static void signal_handler(int) { g_running = 0; }

static constexpr const char* UNIX_SOCKET_PATH = "/tmp/iicpc_pipe.sock";

struct PipeBenchConfig {
    int duration_secs = 10;
    std::size_t num_bots = 1000;
    std::size_t pool_size = 4;  // info only, actual is template param
    bool use_unix = true;
    int exchange_cpu = -1;
    int loadgen_cpu = -1;
    int telemetry_cpu = -1;
};

static PipeBenchConfig parse_args(int argc, char* argv[]) {
    PipeBenchConfig cfg;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            cfg.duration_secs = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--bots") == 0 && i + 1 < argc)
            cfg.num_bots = static_cast<std::size_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--tcp") == 0)
            cfg.use_unix = false;
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
    std::fprintf(stderr, "║   PIPELINE MULTIPLEXED BENCHMARK                        ║\n");
    std::fprintf(stderr, "║   writev() scatter-gather, %zu sockets for %zu bots      ║\n",
                 cfg.pool_size, cfg.num_bots);
    std::fprintf(stderr, "║   Target: <20µs p50, >1M TPS                            ║\n");
    std::fprintf(stderr, "╚══════════════════════════════════════════════════════════╝\n\n");

    // Arena + TSC
    HugePageArena arena;
    (void)arena.init(1024ULL * 1024 * 1024);
    auto tsc_cal = calibrate_tsc();

    // Ring buffer
    using TelemetryRing = SPSCRingBuffer<LatencySample, 1048576>;
    void* ring_mem = arena.allocate_raw(TelemetryRing::TOTAL_SIZE, CACHE_LINE_SIZE);
    TelemetryRing ring(ring_mem, true);

    // Exchange
    UltraExchange exchange;
    std::atomic<bool> exchange_ready{false};
    std::atomic<bool> exchange_stop{false};

    std::thread exchange_thread([&]() {
        if (cfg.exchange_cpu >= 0) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(cfg.exchange_cpu, &cpuset);
            ::sched_setaffinity(0, sizeof(cpuset), &cpuset);
        }

        const char* unix_path = cfg.use_unix ? UNIX_SOCKET_PATH : nullptr;
        if (!exchange.init(unix_path, nullptr, 0, -1)) {
            std::fprintf(stderr, "[exchange] FATAL: init failed\n");
            return;
        }
        exchange_ready.store(true, std::memory_order_release);

        while (!exchange_stop.load(std::memory_order_acquire)) {
            exchange.run_once();
        }
    });

    while (!exchange_ready.load(std::memory_order_acquire)) cpu_pause();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Bot fleet
    BotFleet fleet;
    fleet.init(arena, cfg.num_bots);
    generate_payloads(arena, fleet);

    // Pipeline engine (4 sockets, 512 max batch)
    PipelineEngine<4, 512> engine;
    PipelineConfig<4, 512> engine_config{};
    engine_config.unix_socket_path = cfg.use_unix ? UNIX_SOCKET_PATH : nullptr;
    engine_config.target_host = "127.0.0.1";
    engine_config.target_port = 9990;
    engine_config.cpu_affinity = cfg.loadgen_cpu;

    if (!engine.init(engine_config, fleet, arena)) {
        std::fprintf(stderr, "FATAL: Pipeline engine init failed\n");
        exchange_stop.store(true);
        exchange_thread.join();
        return 1;
    }

    // Telemetry
    TelemetryConsumer consumer;
    ConsumerConfig consumer_config{};
    consumer_config.cpu_affinity = cfg.telemetry_cpu;
    consumer_config.report_interval_ms = 1000;
    consumer.init(consumer_config, tsc_cal);

    std::thread consumer_thread([&]() { consumer.run(ring); });

    // Benchmark
    std::fprintf(stderr, "\n[bench] === PIPELINE BENCHMARK START (%d seconds) ===\n\n",
                 cfg.duration_secs);

    if (cfg.loadgen_cpu >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cfg.loadgen_cpu, &cpuset);
        ::sched_setaffinity(0, sizeof(cpuset), &cpuset);
    }

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(cfg.duration_secs);

    while (g_running && std::chrono::steady_clock::now() < deadline) {
        engine.run_batch(fleet, ring);
    }

    // Drain
    const auto end = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(end - start).count();

    const auto drain_end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < drain_end) {
        for (std::size_t i = 0; i < fleet.count; ++i)
            if (fleet.states[i] == BotState::IDLE)
                fleet.states[i] = BotState::RECEIVED;
        engine.run_batch(fleet, ring);
    }

    consumer.stop();
    if (consumer_thread.joinable()) consumer_thread.join();
    exchange_stop.store(true);
    engine.shutdown();
    if (exchange_thread.joinable()) exchange_thread.join();
    exchange.shutdown();

    // Results
    const uint64_t sends = engine.total_sends();
    const uint64_t recvs = engine.total_recvs();
    const double tps = static_cast<double>(recvs) / elapsed;
    const uint64_t drops = (sends > recvs) ? sends - recvs : 0;
    auto pct = consumer.latest_percentiles();

    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "╔══════════════════════════════════════════════════════════════╗\n");
    std::fprintf(stderr, "║             PIPELINE BENCHMARK RESULTS                      ║\n");
    std::fprintf(stderr, "╠══════════════════════════════════════════════════════════════╣\n");
    std::fprintf(stderr, "║  Duration:     %.2f seconds                                ║\n", elapsed);
    std::fprintf(stderr, "║  Transport:    %-44s  ║\n",
                 cfg.use_unix ? "Unix Domain Socket" : "TCP/IP");
    std::fprintf(stderr, "║  Pool Size:    %-44zu  ║\n", cfg.pool_size);
    std::fprintf(stderr, "║  Bots:         %-44zu  ║\n", cfg.num_bots);
    std::fprintf(stderr, "║                                                              ║\n");
    std::fprintf(stderr, "║  Total Sends:  %-44lu  ║\n", sends);
    std::fprintf(stderr, "║  Total Recvs:  %-44lu  ║\n", recvs);
    std::fprintf(stderr, "║  Drops:        %-44lu  ║\n", drops);
    std::fprintf(stderr, "║  TPS:          %-44.0f  ║\n", tps);
    std::fprintf(stderr, "║                                                              ║\n");
    std::fprintf(stderr, "║  Latency:                                                    ║\n");
    std::fprintf(stderr, "║    min:   %10.1f µs                                       ║\n",
                 static_cast<double>(pct.min) / 1000.0);
    std::fprintf(stderr, "║    p50:   %10.1f µs                                       ║\n",
                 static_cast<double>(pct.p50) / 1000.0);
    std::fprintf(stderr, "║    p90:   %10.1f µs                                       ║\n",
                 static_cast<double>(pct.p90) / 1000.0);
    std::fprintf(stderr, "║    p99:   %10.1f µs                                       ║\n",
                 static_cast<double>(pct.p99) / 1000.0);
    std::fprintf(stderr, "║    p999:  %10.1f µs                                       ║\n",
                 static_cast<double>(pct.p999) / 1000.0);
    std::fprintf(stderr, "║    max:   %10.1f µs                                       ║\n",
                 static_cast<double>(pct.max) / 1000.0);
    std::fprintf(stderr, "║                                                              ║\n");

    const bool tps_ok = tps >= 100'000.0;
    const bool tps_1m = tps >= 1'000'000.0;
    const bool drops_ok = drops == 0;
    const bool p50_5us = pct.p50 <= 5000;
    const bool p50_20us = pct.p50 <= 20000;
    const bool p50_50us = pct.p50 <= 50000;
    const bool p99_100us = pct.p99 <= 100000;

    std::fprintf(stderr, "║  [%s] TPS ≥ 100k                                          ║\n",
                 tps_ok ? "✓" : "✗");
    std::fprintf(stderr, "║  [%s] TPS ≥ 1M                                             ║\n",
                 tps_1m ? "✓" : "✗");
    std::fprintf(stderr, "║  [%s] Zero drops                                           ║\n",
                 drops_ok ? "✓" : "✗");
    std::fprintf(stderr, "║  [%s] p50 < 5µs (HFT-tier)                                ║\n",
                 p50_5us ? "✓" : "✗");
    std::fprintf(stderr, "║  [%s] p50 < 20µs (pipeline-tier)                           ║\n",
                 p50_20us ? "✓" : "✗");
    std::fprintf(stderr, "║  [%s] p50 < 50µs (exchange-tier)                           ║\n",
                 p50_50us ? "✓" : "✗");
    std::fprintf(stderr, "║  [%s] p99 < 100µs (deterministic)                          ║\n",
                 p99_100us ? "✓" : "✗");
    std::fprintf(stderr, "╚══════════════════════════════════════════════════════════════╝\n");

    return 0;
}
