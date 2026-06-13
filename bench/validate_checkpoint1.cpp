// validate_checkpoint1.cpp — Full Stage 1 Pipeline Validation
// Orchestrates: Arena → Ring Buffer → Load Generator → Telemetry Consumer
// Launches dummy exchange as a child process, runs the benchmark for a
// configurable duration, and validates all Checkpoint 1 criteria.

#include "core/arena.hpp"
#include "core/ring_buffer.hpp"
#include "core/tsc.hpp"
#include "core/hdr_histogram.hpp"
#include "core/types.hpp"
#include "loadgen/bot_fleet.hpp"
#include "loadgen/payload_gen.hpp"
#include "loadgen/io_engine.hpp"
#include "telemetry/consumer.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace iicpc;

static volatile sig_atomic_t g_running = 1;
static void signal_handler(int) { g_running = 0; }

// --- Checkpoint validation results ---
struct CheckpointResult {
    bool arena_aligned = false;
    bool ring_cache_lines = false;
    bool tps_achieved = false;
    bool zero_drops = false;
    bool percentiles_valid = false;
    double achieved_tps = 0;
    uint64_t total_samples = 0;
    uint64_t dropped = 0;
};

static void print_results(const CheckpointResult& r) {
    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "--- CHECKPOINT 1 VALIDATION RESULTS ---\n");
    std::fprintf(stderr, "--- [%s] Arena: 64-byte aligned allocations ---\n",
                 r.arena_aligned ? "✓" : "✗");
    std::fprintf(stderr, "--- [%s] Ring buffer: indices on separate cache lines ---\n",
                 r.ring_cache_lines ? "✓" : "✗");
    std::fprintf(stderr, "--- [%s] Load generator: ≥100,000 TPS (achieved: %.0f) ---\n",
                 r.tps_achieved ? "✓" : "✗", r.achieved_tps);
    std::fprintf(stderr, "--- [%s] Telemetry: zero dropped packets (%lu dropped) ---\n",
                 r.zero_drops ? "✓" : "✗", r.dropped);
    std::fprintf(stderr, "--- [%s] Telemetry: valid percentile output ---\n",
                 r.percentiles_valid ? "✓" : "✗");

    const bool all_pass = r.arena_aligned && r.ring_cache_lines &&
                          r.tps_achieved && r.zero_drops && r.percentiles_valid;
    std::fprintf(stderr, "--- OVERALL: %s ---\n",
                 all_pass ? "✅ PASS" : "❌ FAIL");
}

int main(int argc, char* argv[]) {
    int duration_secs = 10;
    std::size_t num_bots = 1000;
    uint16_t port = 9999;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            duration_secs = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--bots") == 0 && i + 1 < argc)
            num_bots = static_cast<std::size_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
    }

    ::signal(SIGINT, signal_handler);
    ::signal(SIGTERM, signal_handler);
    ::signal(SIGPIPE, SIG_IGN);

    CheckpointResult result{};

    // Step 1: Initialize the arena
    std::fprintf(stderr, "\n=== Step 1: Arena Allocator ===\n");
    HugePageArena arena;
    (void)arena.init(512 * 1024 * 1024); // 512 MiB for validation

    // Verify alignment
    void* test_ptr1 = arena.allocate_raw(128);
    void* test_ptr2 = arena.allocate_raw(7);   // Odd size
    void* test_ptr3 = arena.allocate_raw(1);
    result.arena_aligned =
        (reinterpret_cast<uintptr_t>(test_ptr1) % CACHE_LINE_SIZE == 0) &&
        (reinterpret_cast<uintptr_t>(test_ptr2) % CACHE_LINE_SIZE == 0) &&
        (reinterpret_cast<uintptr_t>(test_ptr3) % CACHE_LINE_SIZE == 0);
    std::fprintf(stderr, "[check] Arena alignment: %s\n",
                 result.arena_aligned ? "PASS" : "FAIL");
    arena.reset();

    // Step 2: Calibrate TSC
    std::fprintf(stderr, "\n=== Step 2: TSC Calibration ===\n");
    auto tsc_cal = calibrate_tsc();

    // Step 3: Create ring buffer and verify cache line separation
    std::fprintf(stderr, "\n=== Step 3: Ring Buffer ===\n");
    using TelemetryRing = SPSCRingBuffer<LatencySample, 1048576>;

    // Allocate ring buffer memory from arena
    void* ring_mem = arena.allocate_raw(TelemetryRing::TOTAL_SIZE, CACHE_LINE_SIZE);
    TelemetryRing ring(ring_mem, true);

    result.ring_cache_lines = TelemetryRing::verify_cache_line_separation(ring_mem);
    std::fprintf(stderr, "[check] Ring buffer cache line separation: %s\n",
                 result.ring_cache_lines ? "PASS" : "FAIL");

    // Step 4: Start dummy exchange (child process)
    std::fprintf(stderr, "\n=== Step 4: Starting Dummy Exchange ===\n");
    pid_t exchange_pid = fork();
    if (exchange_pid == 0) {
        // Child: exec the dummy exchange
        char port_str[16];
        std::snprintf(port_str, sizeof(port_str), "%u", port);
        ::execlp("./dummy_exchange", "dummy_exchange", "--port", port_str, nullptr);
        // If exec fails, run a minimal inline server
        std::fprintf(stderr, "[exchange] exec failed, cannot run exchange\n");
        ::_exit(1);
    }

    // Wait for exchange to be ready
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Step 5: Initialize bot fleet and payloads
    std::fprintf(stderr, "\n=== Step 5: Bot Fleet (%zu bots) ===\n", num_bots);
    BotFleet fleet;
    if (!fleet.init(arena, num_bots)) {
        std::fprintf(stderr, "FATAL: Failed to init bot fleet\n");
        kill(exchange_pid, SIGTERM);
        return 1;
    }

    if (!generate_payloads(arena, fleet)) {
        std::fprintf(stderr, "FATAL: Failed to generate payloads\n");
        kill(exchange_pid, SIGTERM);
        return 1;
    }

    // Step 6: Initialize I/O engine
    std::fprintf(stderr, "\n=== Step 6: I/O Engine ===\n");
    IoEngine* engine = create_io_engine(arena);
    IoEngineConfig io_config{};
    io_config.target_host = "127.0.0.1";
    io_config.target_port = port;
    io_config.batch_size = 256;
    io_config.target_tps = 100'000;

    if (!engine->init(io_config, fleet, arena)) {
        std::fprintf(stderr, "FATAL: Failed to init I/O engine\n");
        kill(exchange_pid, SIGTERM);
        return 1;
    }

    // Step 7: Start telemetry consumer thread
    std::fprintf(stderr, "\n=== Step 7: Telemetry Consumer ===\n");
    TelemetryConsumer consumer;
    ConsumerConfig consumer_config{};
    consumer_config.report_interval_ms = 1000;
    consumer.init(consumer_config, tsc_cal);

    std::thread consumer_thread([&]() {
        consumer.run(ring);
    });

    // Step 8: RUN THE BENCHMARK
    std::fprintf(stderr, "\n=== Step 8: BENCHMARK (%d seconds) ===\n", duration_secs);
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(duration_secs);

    while (g_running && std::chrono::steady_clock::now() < deadline) {
        engine->run_batch(fleet, ring);
    }

    // Step 9: Shutdown and collect results
    std::fprintf(stderr, "\n=== Step 9: Shutdown ===\n");
    const auto end = std::chrono::steady_clock::now();
    const double elapsed_secs = std::chrono::duration<double>(end - start).count();

    // Allow a brief drain period for in-flight packets
    // Keep calling run_batch to receive remaining ACKs
    std::fprintf(stderr, "[shutdown] Draining in-flight packets (1s)...\n");
    const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < drain_deadline) {
        // Mark all bots as non-IDLE so run_batch only does receives, not sends
        for (std::size_t i = 0; i < fleet.count; ++i) {
            if (fleet.states[i] == BotState::IDLE) {
                fleet.states[i] = BotState::RECEIVED; // Prevent further sends
            }
        }
        engine->run_batch(fleet, ring);
    }

    consumer.stop();
    if (consumer_thread.joinable()) consumer_thread.join();

    engine->shutdown();

    // Kill exchange
    if (exchange_pid > 0) {
        kill(exchange_pid, SIGTERM);
        int status;
        waitpid(exchange_pid, &status, 0);
    }

    // Step 10: Validate results
    // Ground truth: compare engine sends vs receives
    const uint64_t actual_drops = engine->total_sends() - engine->total_recvs();
    result.total_samples = consumer.total_samples();
    result.dropped = actual_drops;
    result.achieved_tps = static_cast<double>(engine->total_recvs()) / elapsed_secs;
    result.tps_achieved = result.achieved_tps >= 100'000.0;
    result.zero_drops = actual_drops == 0;

    auto pct = consumer.latest_percentiles();
    result.percentiles_valid = (pct.total_count > 0) &&
                               (pct.p50 > 0) && (pct.p50 <= pct.p90) &&
                               (pct.p90 <= pct.p99) && (pct.p99 <= pct.p999);

    std::fprintf(stderr, "\nElapsed: %.2f seconds\n", elapsed_secs);
    std::fprintf(stderr, "Total sends: %lu\n", engine->total_sends());
    std::fprintf(stderr, "Total recvs: %lu\n", engine->total_recvs());
    std::fprintf(stderr, "Achieved TPS: %.0f\n", result.achieved_tps);

    print_results(result);

    return (result.arena_aligned && result.ring_cache_lines &&
            result.tps_achieved && result.zero_drops && result.percentiles_valid)
        ? 0 : 1;
}
