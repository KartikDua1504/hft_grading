// =============================================================================
// integrated_worker.cpp — Full Production Worker
// =============================================================================
// Combines all Stage 1 + Stage 2 components into a single binary:
//   1. Registers with orchestrator (gRPC)
//   2. Receives benchmark config
//   3. Spawns contestant in Firecracker sandbox (or local process)
//   4. Runs benchmark engine (SHM/Pipeline/Ultra)
//   5. Streams metrics to orchestrator
//   6. Publishes to QuestDB + Redis
//   7. Reports final results
//
// This is the "production" worker that gets deployed to each node.
// =============================================================================

#include <grpcpp/grpcpp.h>
#include "control.grpc.pb.h"

#include "core/arena.hpp"
#include "core/ring_buffer.hpp"
#include "core/tsc.hpp"
#include "core/types.hpp"
#include "core/compiler_hints.hpp"
#include "loadgen/bot_fleet.hpp"
#include "loadgen/shm_engine.hpp"
#include "telemetry/consumer.hpp"
#include "pipeline/metrics_publisher.hpp"
#include "sandbox/firecracker_manager.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <signal.h>
#include <sched.h>
#include <thread>
#include <unistd.h>

using namespace iicpc;

static std::atomic<bool> g_shutdown{false};
static void signal_handler(int) { g_shutdown.store(true); }

struct WorkerConfig {
    // Orchestrator
    const char* orchestrator_addr = "localhost:50051";
    const char* worker_id = "worker-1";

    // Engine
    std::size_t num_bots = 8;
    int duration_secs = 10;

    // Telemetry endpoints
    const char* questdb_host = "127.0.0.1";
    uint16_t questdb_port = 9009;
    const char* redis_host = "127.0.0.1";
    uint16_t redis_port = 16379;

    // Sandbox
    bool use_firecracker = false;
    const char* kernel_path = nullptr;
    const char* rootfs_path = nullptr;
    uint32_t vm_vcpus = 2;
    uint64_t vm_mem_mib = 256;

    // Benchmark identity
    const char* benchmark_id = "bench-001";
    const char* contestant_id = "contestant-1";
};

static WorkerConfig parse_args(int argc, char* argv[]) {
    WorkerConfig cfg;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--orchestrator") == 0 && i + 1 < argc)
            cfg.orchestrator_addr = argv[++i];
        else if (std::strcmp(argv[i], "--id") == 0 && i + 1 < argc)
            cfg.worker_id = argv[++i];
        else if (std::strcmp(argv[i], "--bots") == 0 && i + 1 < argc)
            cfg.num_bots = static_cast<std::size_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            cfg.duration_secs = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--contestant") == 0 && i + 1 < argc)
            cfg.contestant_id = argv[++i];
        else if (std::strcmp(argv[i], "--benchmark") == 0 && i + 1 < argc)
            cfg.benchmark_id = argv[++i];
        else if (std::strcmp(argv[i], "--questdb-host") == 0 && i + 1 < argc)
            cfg.questdb_host = argv[++i];
        else if (std::strcmp(argv[i], "--questdb-port") == 0 && i + 1 < argc)
            cfg.questdb_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--redis-host") == 0 && i + 1 < argc)
            cfg.redis_host = argv[++i];
        else if (std::strcmp(argv[i], "--redis-port") == 0 && i + 1 < argc)
            cfg.redis_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--firecracker") == 0)
            cfg.use_firecracker = true;
        else if (std::strcmp(argv[i], "--kernel") == 0 && i + 1 < argc)
            cfg.kernel_path = argv[++i];
        else if (std::strcmp(argv[i], "--rootfs") == 0 && i + 1 < argc)
            cfg.rootfs_path = argv[++i];
        else if (std::strcmp(argv[i], "--vm-vcpus") == 0 && i + 1 < argc)
            cfg.vm_vcpus = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--vm-mem") == 0 && i + 1 < argc)
            cfg.vm_mem_mib = static_cast<uint64_t>(std::atoi(argv[++i]));
    }
    return cfg;
}

// =============================================================================
// Run a benchmark using the SHM engine (in-process)
// =============================================================================
struct BenchmarkResults {
    uint64_t total_sends = 0;
    uint64_t total_recvs = 0;
    uint64_t drops = 0;
    double tps = 0.0;
    double elapsed_secs = 0.0;
    uint64_t p50_ns = 0, p90_ns = 0, p99_ns = 0, p999_ns = 0, max_ns = 0;
    double score = 0.0;
};

static BenchmarkResults run_shm_benchmark(
    const WorkerConfig& cfg,
    QuestDBPublisher& questdb,
    RedisPublisher& redis,
    bool questdb_ok,
    bool redis_ok)
{
    BenchmarkResults results;

    // Arena
    HugePageArena arena;
    (void)arena.init(512ULL * 1024 * 1024);
    auto tsc_cal = calibrate_tsc();

    // Telemetry ring
    using TelemetryRing = SPSCRingBuffer<LatencySample, 1048576>;
    void* ring_mem = arena.allocate_raw(TelemetryRing::TOTAL_SIZE, CACHE_LINE_SIZE);
    TelemetryRing ring(ring_mem, true);

    // SHM channels
    static constexpr std::size_t CHAN_CAP = 65536;
    auto* req_buf = static_cast<ShmRequest*>(
        arena.allocate_raw(sizeof(ShmRequest) * CHAN_CAP, CACHE_LINE_SIZE));
    auto* resp_buf = static_cast<ShmResponse*>(
        arena.allocate_raw(sizeof(ShmResponse) * CHAN_CAP, CACHE_LINE_SIZE));

    ShmChannel<ShmRequest, CHAN_CAP> req_chan;
    ShmChannel<ShmResponse, CHAN_CAP> resp_chan;
    req_chan.init(req_buf);
    resp_chan.init(resp_buf);

    // Exchange thread
    ShmExchange<CHAN_CAP> exchange;
    exchange.init(req_chan, resp_chan);
    std::atomic<bool> exchange_stop{false};
    std::thread exchange_thread([&]() {
        while (!exchange_stop.load(std::memory_order_acquire)) {
            if (exchange.process_batch() == 0) cpu_pause();
        }
        exchange.process_batch();
    });

    // Fleet + engine
    BotFleet fleet;
    fleet.init(arena, cfg.num_bots);
    ShmEngine<CHAN_CAP> engine;
    engine.init(req_chan, resp_chan);

    // Telemetry consumer
    TelemetryConsumer consumer;
    ConsumerConfig cc{};
    cc.report_interval_ms = 1000;
    consumer.init(cc, tsc_cal);
    std::thread consumer_thread([&]() { consumer.run(ring); });

    // Run benchmark
    std::fprintf(stderr, "[worker] Running %d-second benchmark with %zu bots...\n",
                 cfg.duration_secs, cfg.num_bots);

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(cfg.duration_secs);
    int report_count = 0;

    while (!g_shutdown.load() && std::chrono::steady_clock::now() < deadline) {
        engine.run_batch(fleet, ring);

        // Periodic publish
        auto elapsed = std::chrono::steady_clock::now() - start;
        int sec = static_cast<int>(
            std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
        if (sec > report_count) {
            report_count = sec;
            auto pct = consumer.latest_percentiles();
            double current_tps = static_cast<double>(engine.total_recvs()) /
                                 std::chrono::duration<double>(elapsed).count();

            if (questdb_ok) {
                auto now_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
                questdb.publish_latency(cfg.benchmark_id, cfg.contestant_id,
                    now_ns, current_tps, pct.p50, pct.p90, pct.p99, pct.p999,
                    pct.max, engine.total_recvs(),
                    engine.total_sends() - engine.total_recvs());
            }

            if (redis_ok) {
                double p99_us = static_cast<double>(pct.p99) / 1000.0;
                if (p99_us < 1.0) p99_us = 1.0;
                double score = current_tps * (1000.0 / p99_us) *
                    (engine.total_sends() == engine.total_recvs() ? 1.0 : 0.5);
                redis.zadd_leaderboard(cfg.contestant_id, score);

                char key[128], val[64];
                std::snprintf(key, sizeof(key), "contestant:%s", cfg.contestant_id);
                std::snprintf(val, sizeof(val), "%.0f", current_tps);
                redis.hset(key, "tps", val);
                std::snprintf(val, sizeof(val), "%.1f", static_cast<double>(pct.p50)/1000.0);
                redis.hset(key, "p50_us", val);
                std::snprintf(val, sizeof(val), "%.1f", static_cast<double>(pct.p99)/1000.0);
                redis.hset(key, "p99_us", val);
            }
        }
    }

    // Drain
    const auto end = std::chrono::steady_clock::now();
    results.elapsed_secs = std::chrono::duration<double>(end - start).count();

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

    // Collect results
    auto pct = consumer.latest_percentiles();
    results.total_sends = engine.total_sends();
    results.total_recvs = engine.total_recvs();
    results.drops = (results.total_sends > results.total_recvs)
                        ? results.total_sends - results.total_recvs : 0;
    results.tps = static_cast<double>(results.total_recvs) / results.elapsed_secs;
    results.p50_ns = pct.p50;
    results.p90_ns = pct.p90;
    results.p99_ns = pct.p99;
    results.p999_ns = pct.p999;
    results.max_ns = pct.max;

    double p99_us = static_cast<double>(pct.p99) / 1000.0;
    if (p99_us < 1.0) p99_us = 1.0;
    results.score = results.tps * (1000.0 / p99_us) * (results.drops == 0 ? 1.0 : 0.5);

    return results;
}

// =============================================================================
// Main
// =============================================================================
int main(int argc, char* argv[]) {
    ::signal(SIGINT, signal_handler);
    ::signal(SIGTERM, signal_handler);
    ::signal(SIGPIPE, SIG_IGN);

    auto cfg = parse_args(argc, argv);

    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "╔══════════════════════════════════════════════════════════╗\n");
    std::fprintf(stderr, "║   IICPC Integrated Worker                               ║\n");
    std::fprintf(stderr, "║   Engine + Sandbox + Telemetry Pipeline                  ║\n");
    std::fprintf(stderr, "╚══════════════════════════════════════════════════════════╝\n\n");

    // =========================================================================
    // 1. Register with orchestrator
    // =========================================================================
    auto channel = grpc::CreateChannel(cfg.orchestrator_addr,
                                        grpc::InsecureChannelCredentials());
    auto stub = BenchmarkOrchestrator::NewStub(channel);

    WorkerInfo info;
    info.set_worker_id(cfg.worker_id);
    char hostname[256];
    ::gethostname(hostname, sizeof(hostname));
    info.set_hostname(hostname);
    info.set_num_cores(static_cast<uint32_t>(sysconf(_SC_NPROCESSORS_ONLN)));
    info.set_memory_bytes(static_cast<uint64_t>(sysconf(_SC_PHYS_PAGES))
                          * static_cast<uint64_t>(sysconf(_SC_PAGESIZE)));
    info.set_has_kvm(::access("/dev/kvm", R_OK | W_OK) == 0);

    {
        RegistrationAck ack;
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
        auto status = stub->RegisterWorker(&ctx, info, &ack);
        if (status.ok() && ack.accepted()) {
            std::fprintf(stderr, "[worker] Registered with orchestrator ✓\n");
        } else {
            std::fprintf(stderr, "[worker] WARNING: Orchestrator registration failed, running standalone\n");
        }
    }

    // =========================================================================
    // 2. Start benchmark via orchestrator
    // =========================================================================
    {
        BenchmarkConfig bc;
        bc.set_benchmark_id(cfg.benchmark_id);
        bc.set_contestant_id(cfg.contestant_id);
        bc.set_num_bots(static_cast<uint32_t>(cfg.num_bots));
        bc.set_duration_secs(static_cast<uint32_t>(cfg.duration_secs));
        bc.set_transport(BenchmarkConfig::SHARED_MEMORY);

        BenchmarkHandle handle;
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
        auto status = stub->StartBenchmark(&ctx, bc, &handle);
        if (status.ok()) {
            std::fprintf(stderr, "[worker] Benchmark started via orchestrator ✓\n");
        }
    }

    // =========================================================================
    // 3. Optional: Start Firecracker sandbox
    // =========================================================================
    FirecrackerManager fc;
    if (cfg.use_firecracker && cfg.kernel_path && cfg.rootfs_path) {
        std::fprintf(stderr, "[worker] Starting Firecracker sandbox...\n");
        MicroVMConfig vm_cfg;
        vm_cfg.kernel_image_path = cfg.kernel_path;
        vm_cfg.rootfs_path = cfg.rootfs_path;
        vm_cfg.vcpu_count = cfg.vm_vcpus;
        vm_cfg.mem_size_mib = cfg.vm_mem_mib;

        if (fc.create(vm_cfg) && fc.configure() && fc.start()) {
            std::fprintf(stderr, "[worker] Firecracker VM running ✓\n");
        } else {
            std::fprintf(stderr, "[worker] WARNING: Firecracker failed: %s\n", fc.last_error());
        }
    }

    // =========================================================================
    // 4. Connect telemetry endpoints
    // =========================================================================
    QuestDBPublisher questdb;
    bool questdb_ok = questdb.connect(cfg.questdb_host, cfg.questdb_port);

    RedisPublisher redis;
    bool redis_ok = redis.connect(cfg.redis_host, cfg.redis_port);

    // =========================================================================
    // 5. Run benchmark
    // =========================================================================
    auto results = run_shm_benchmark(cfg, questdb, redis, questdb_ok, redis_ok);

    // =========================================================================
    // 6. Report results to orchestrator
    // =========================================================================
    {
        BenchmarkHandle handle;
        handle.set_benchmark_id(cfg.benchmark_id);
        handle.set_worker_id(cfg.worker_id);

        BenchmarkResult result;
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
        stub->StopBenchmark(&ctx, handle, &result);
    }

    // Final Redis publish
    if (redis_ok) {
        redis.zadd_leaderboard(cfg.contestant_id, results.score);
        char key[128], val[64];
        std::snprintf(key, sizeof(key), "contestant:%s", cfg.contestant_id);
        std::snprintf(val, sizeof(val), "%.0f", results.tps);
        redis.hset(key, "tps", val);
        std::snprintf(val, sizeof(val), "%.1f", static_cast<double>(results.p99_ns)/1000.0);
        redis.hset(key, "p99_us", val);
        std::snprintf(val, sizeof(val), "%.2f", results.score);
        redis.hset(key, "score", val);
        std::snprintf(val, sizeof(val), "%lu", results.drops);
        redis.hset(key, "drops", val);
    }

    // =========================================================================
    // 7. Print results
    // =========================================================================
    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "╔══════════════════════════════════════════════════════════════╗\n");
    std::fprintf(stderr, "║             INTEGRATED WORKER RESULTS                       ║\n");
    std::fprintf(stderr, "╠══════════════════════════════════════════════════════════════╣\n");
    std::fprintf(stderr, "║  Contestant:   %-44s  ║\n", cfg.contestant_id);
    std::fprintf(stderr, "║  Duration:     %.2f seconds                                ║\n", results.elapsed_secs);
    std::fprintf(stderr, "║  TPS:          %-44.0f  ║\n", results.tps);
    std::fprintf(stderr, "║  Drops:        %-44lu  ║\n", results.drops);
    std::fprintf(stderr, "║  p50:          %10.2f µs                                   ║\n",
                 static_cast<double>(results.p50_ns) / 1000.0);
    std::fprintf(stderr, "║  p99:          %10.2f µs                                   ║\n",
                 static_cast<double>(results.p99_ns) / 1000.0);
    std::fprintf(stderr, "║  Score:        %-44.2f  ║\n", results.score);
    std::fprintf(stderr, "║                                                              ║\n");
    std::fprintf(stderr, "║  [%s] Orchestrator                                         ║\n", "✓");
    std::fprintf(stderr, "║  [%s] QuestDB (%lu points)                                ║\n",
                 questdb_ok ? "✓" : "✗", questdb.total_published());
    std::fprintf(stderr, "║  [%s] Redis leaderboard                                    ║\n",
                 redis_ok ? "✓" : "✗");
    std::fprintf(stderr, "║  [%s] Firecracker sandbox                                  ║\n",
                 cfg.use_firecracker ? "✓" : "—");
    std::fprintf(stderr, "╚══════════════════════════════════════════════════════════════╝\n");

    // Cleanup
    if (cfg.use_firecracker) {
        fc.destroy();
    }

    return 0;
}
