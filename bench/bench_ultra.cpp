// =============================================================================
// bench_ultra.cpp — Ultra-Low-Latency Benchmark
// =============================================================================
// Single-binary benchmark: exchange, loadgen, and telemetry all in one process
// with separate pinned threads. Uses Unix domain sockets to bypass the TCP/IP
// stack (no checksumming, no Nagle, no congestion control). Note: UDS still
// transits the kernel's VFS layer — true kernel bypass requires SHM or DPDK.
//
// This is the "squeeze everything" benchmark.
// Target: <5µs p50, >1M TPS
// =============================================================================

#include "core/arena.hpp"
#include "core/ring_buffer.hpp"
#include "core/tsc.hpp"
#include "core/hdr_histogram.hpp"
#include "core/types.hpp"
#include "core/compiler_hints.hpp"
#include "loadgen/bot_fleet.hpp"
#include "loadgen/payload_gen.hpp"
#include "loadgen/ultra_engine.hpp"
#include "telemetry/consumer.hpp"

#include <atomic>
#include <cerrno>
#include <charconv>
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

// =============================================================================
// Configuration — everything is compile-time where possible
// =============================================================================
static constexpr const char* UNIX_SOCKET_PATH = "/tmp/iicpc_ultra.sock";
static constexpr uint16_t TCP_PORT = 9990;

struct BenchConfig {
    int duration_secs = 10;
    std::size_t num_bots = 1000;
    bool use_unix = true;       // Unix domain sockets (fastest)
    bool use_tcp = false;       // TCP (for comparison)
    int exchange_cpu = -1;      // Core for exchange (-1 = no pin)
    int loadgen_cpu = -1;       // Core for loadgen
    int telemetry_cpu = -1;     // Core for telemetry
};

// Safe integer parsing — std::atoi is UB on overflow and has zero error checking.
// std::from_chars is zero-allocation, extremely fast, and bounds-safe.
static int safe_parse_int(const char* str, int fallback = 0) {
    int val = fallback;
    auto [ptr, ec] = std::from_chars(str, str + std::strlen(str), val);
    return (ec == std::errc{}) ? val : fallback;
}

static BenchConfig parse_args(int argc, char* argv[]) {
    BenchConfig cfg;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            cfg.duration_secs = safe_parse_int(argv[++i], cfg.duration_secs);
        else if (std::strcmp(argv[i], "--bots") == 0 && i + 1 < argc)
            cfg.num_bots = static_cast<std::size_t>(safe_parse_int(argv[++i], static_cast<int>(cfg.num_bots)));
        else if (std::strcmp(argv[i], "--tcp") == 0)
            { cfg.use_tcp = true; cfg.use_unix = false; }
        else if (std::strcmp(argv[i], "--unix") == 0)
            { cfg.use_unix = true; cfg.use_tcp = false; }
        else if (std::strcmp(argv[i], "--both") == 0)
            { cfg.use_unix = true; cfg.use_tcp = true; }
        else if (std::strcmp(argv[i], "--exchange-cpu") == 0 && i + 1 < argc)
            cfg.exchange_cpu = safe_parse_int(argv[++i], cfg.exchange_cpu);
        else if (std::strcmp(argv[i], "--loadgen-cpu") == 0 && i + 1 < argc)
            cfg.loadgen_cpu = safe_parse_int(argv[++i], cfg.loadgen_cpu);
        else if (std::strcmp(argv[i], "--telemetry-cpu") == 0 && i + 1 < argc)
            cfg.telemetry_cpu = safe_parse_int(argv[++i], cfg.telemetry_cpu);
    }
    return cfg;
}

int main(int argc, char* argv[]) {
    auto cfg = parse_args(argc, argv);

    ::signal(SIGINT, signal_handler);
    ::signal(SIGTERM, signal_handler);
    ::signal(SIGPIPE, SIG_IGN);

    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "╔══════════════════════════════════════════════════╗\n");
    std::fprintf(stderr, "║   ULTRA-LOW-LATENCY BENCHMARK                   ║\n");
    std::fprintf(stderr, "║   Target: <5µs p50, >1M TPS                     ║\n");
    std::fprintf(stderr, "╚══════════════════════════════════════════════════╝\n\n");

    // =========================================================================
    // Step 1: Arena + TSC calibration
    // =========================================================================
    HugePageArena arena;
    (void)arena.init(1024ULL * 1024 * 1024); // 1 GiB

    auto tsc_cal = calibrate_tsc();

    // =========================================================================
    // Step 2: Ring buffer
    // =========================================================================
    using TelemetryRing = SPSCRingBuffer<LatencySample, 1048576>;
    void* ring_mem = arena.allocate_raw(TelemetryRing::TOTAL_SIZE, CACHE_LINE_SIZE);
    TelemetryRing ring(ring_mem, true);

    // =========================================================================
    // Step 3: Start exchange thread
    // =========================================================================
    UltraExchange exchange;
    // Pad cross-thread atomics to separate cache lines to prevent false sharing.
    // Without padding, spinning on exchange_stop while another thread writes
    // exchange_ready causes MESI cache line bouncing (~50ns per access).
    alignas(64) std::atomic<bool> exchange_ready{false};
    alignas(64) std::atomic<bool> exchange_stop{false};

    std::thread exchange_thread([&]() {
        if (cfg.exchange_cpu >= 0) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(cfg.exchange_cpu, &cpuset);
            ::sched_setaffinity(0, sizeof(cpuset), &cpuset);
            std::fprintf(stderr, "[exchange] Pinned to CPU %d\n", cfg.exchange_cpu);
        }

        const char* unix_path = cfg.use_unix ? UNIX_SOCKET_PATH : nullptr;
        const char* tcp_bind = cfg.use_tcp ? "127.0.0.1" : nullptr;
        uint16_t tcp_port = cfg.use_tcp ? TCP_PORT : 0;

        if (!exchange.init(unix_path, tcp_bind, tcp_port, -1)) {
            std::fprintf(stderr, "[exchange] FATAL: init failed\n");
            return;
        }

        exchange_ready.store(true, std::memory_order_release);

        // Busy-loop exchange
        while (!exchange_stop.load(std::memory_order_acquire)) {
            exchange.run_once();
        }
    });

    // Wait for exchange to be ready
    while (!exchange_ready.load(std::memory_order_acquire)) {
        cpu_pause();
    }
    // Small delay for listener socket to be fully up
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // =========================================================================
    // Step 4: Bot fleet + payloads
    // =========================================================================
    std::fprintf(stderr, "[bench] Initializing %zu bots...\n", cfg.num_bots);
    BotFleet fleet;
    fleet.init(arena, cfg.num_bots);
    generate_payloads(arena, fleet);

    // =========================================================================
    // Step 5: Ultra engine
    // =========================================================================
    UltraEngine engine;
    UltraEngineConfig engine_config{};
    engine_config.target_host = "127.0.0.1";
    engine_config.target_port = TCP_PORT;
    engine_config.unix_socket_path = cfg.use_unix ? UNIX_SOCKET_PATH : nullptr;
    engine_config.cpu_affinity = cfg.loadgen_cpu;
    engine_config.target_tps = 1'000'000;

    if (!engine.init(engine_config, fleet, arena)) {
        std::fprintf(stderr, "FATAL: Engine init failed\n");
        exchange_stop.store(true, std::memory_order_release);
        exchange_thread.join();
        return 1;
    }

    // =========================================================================
    // Step 6: Telemetry consumer thread
    // =========================================================================
    TelemetryConsumer consumer;
    ConsumerConfig consumer_config{};
    consumer_config.cpu_affinity = cfg.telemetry_cpu;
    consumer_config.report_interval_ms = 1000;
    consumer.init(consumer_config, tsc_cal);

    std::thread consumer_thread([&]() {
        consumer.run(ring);
    });

    // =========================================================================
    // Step 7: RUN THE BENCHMARK
    // =========================================================================
    std::fprintf(stderr, "\n[bench] === BENCHMARK START (%d seconds) ===\n\n",
                 cfg.duration_secs);

    // Pin loadgen thread if requested
    if (cfg.loadgen_cpu >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cfg.loadgen_cpu, &cpuset);
        ::sched_setaffinity(0, sizeof(cpuset), &cpuset);
        std::fprintf(stderr, "[bench] Loadgen pinned to CPU %d\n", cfg.loadgen_cpu);
    }

    // Calculate TSC deadline — avoid std::chrono::now() in the hot loop.
    // __rdtsc() is a single hardware instruction (~1 cycle) vs clock_gettime
    // (~20-30ns even via vDSO), and avoids I-cache pollution.
    const uint64_t tsc_start = __rdtsc();
    const uint64_t tsc_deadline = tsc_start +
        static_cast<uint64_t>(cfg.duration_secs) * tsc_cal.tsc_hz;

    while (g_running && __rdtsc() < tsc_deadline) {
        engine.run_batch(fleet, ring);
    }

    // Use TSC delta for precise elapsed time
    const uint64_t tsc_end = __rdtsc();
    const double elapsed_secs = static_cast<double>(tsc_end - tsc_start) * tsc_cal.tsc_to_ns / 1e9;

    std::fprintf(stderr, "\n[bench] Draining...\n");
    const auto drain_end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < drain_end) {
        for (std::size_t i = 0; i < fleet.count; ++i) {
            if (fleet.states[i] == BotState::IDLE)
                fleet.states[i] = BotState::RECEIVED;
        }
        engine.run_batch(fleet, ring);
    }

    consumer.stop();
    if (consumer_thread.joinable()) consumer_thread.join();

    exchange_stop.store(true, std::memory_order_release);
    engine.shutdown();
    if (exchange_thread.joinable()) exchange_thread.join();
    exchange.shutdown();

    // =========================================================================
    // Step 9: Results
    // =========================================================================
    const uint64_t total_sends = engine.total_sends();
    const uint64_t total_recvs = engine.total_recvs();
    const double tps = static_cast<double>(total_recvs) / elapsed_secs;
    const uint64_t drops = total_sends - total_recvs;

    auto pct = consumer.latest_percentiles();

    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "╔══════════════════════════════════════════════════════════════╗\n");
    std::fprintf(stderr, "║               ULTRA BENCHMARK RESULTS                       ║\n");
    std::fprintf(stderr, "╠══════════════════════════════════════════════════════════════╣\n");
    std::fprintf(stderr, "║  Duration:     %.2f seconds                                ║\n", elapsed_secs);
    std::fprintf(stderr, "║  Transport:    %-44s  ║\n",
                 cfg.use_unix ? "Unix Domain Socket (bypasses TCP/IP stack)" : "TCP/IP");
    std::fprintf(stderr, "║  Bots:         %-44zu  ║\n", cfg.num_bots);
    std::fprintf(stderr, "║                                                              ║\n");
    std::fprintf(stderr, "║  Total Sends:  %-44lu  ║\n", total_sends);
    std::fprintf(stderr, "║  Total Recvs:  %-44lu  ║\n", total_recvs);
    std::fprintf(stderr, "║  Drops:        %-44lu  ║\n", drops);
    std::fprintf(stderr, "║  TPS:          %-44.0f  ║\n", tps);
    std::fprintf(stderr, "║                                                              ║\n");
    std::fprintf(stderr, "║  Latency:                                                    ║\n");
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

    // Verdicts
    const bool tps_ok = tps >= 100'000.0;
    const bool tps_1m = tps >= 1'000'000.0;
    const bool drops_ok = drops == 0;
    const bool p50_5us = pct.p50 <= 5000;   // 5µs in ns
    const bool p50_50us = pct.p50 <= 50000;  // 50µs
    const bool p99_det = pct.p99 <= 100000;  // 100µs deterministic

    std::fprintf(stderr, "║  [%s] TPS ≥ 100k                                          ║\n",
                 tps_ok ? "✓" : "✗");
    std::fprintf(stderr, "║  [%s] TPS ≥ 1M                                             ║\n",
                 tps_1m ? "✓" : "✗");
    std::fprintf(stderr, "║  [%s] Zero drops                                           ║\n",
                 drops_ok ? "✓" : "✗");
    std::fprintf(stderr, "║  [%s] p50 < 5µs (HFT-tier)                                ║\n",
                 p50_5us ? "✓" : "✗");
    std::fprintf(stderr, "║  [%s] p50 < 50µs (exchange-tier)                           ║\n",
                 p50_50us ? "✓" : "✗");
    std::fprintf(stderr, "║  [%s] p99 < 100µs (deterministic)                          ║\n",
                 p99_det ? "✓" : "✗");
    std::fprintf(stderr, "╚══════════════════════════════════════════════════════════════╝\n");

    return 0;
}
