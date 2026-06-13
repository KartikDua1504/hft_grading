// pipeline_e2e.cpp — End-to-End Pipeline Test
// Runs a short SHM benchmark and publishes results to:
//   - QuestDB (ILP on port 9009) — time-series latency data
//   - Redis (RESP on port 16379) — leaderboard sorted set
//
// This validates the full data flow:
//   Engine → Telemetry → QuestDB + Redis

#include "core/arena.hpp"
#include "core/ring_buffer.hpp"
#include "core/tsc.hpp"
#include "core/types.hpp"
#include "core/compiler_hints.hpp"
#include "loadgen/bot_fleet.hpp"
#include "loadgen/shm_engine.hpp"
#include "telemetry/consumer.hpp"
#include "pipeline/metrics_publisher.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <signal.h>
#include <sched.h>
#include <thread>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

using namespace iicpc;

static volatile sig_atomic_t g_running = 1;
static void signal_handler(int) { g_running = 0; }

struct PipelineConfig {
    int duration_secs = 5;
    std::size_t num_bots = 8;
    const char* questdb_host = "127.0.0.1";
    uint16_t questdb_port = 9009;
    const char* redis_host = "127.0.0.1";
    uint16_t redis_port = 16379;
    const char* benchmark_id = "bench-e2e-001";
    const char* contestant_id = "contestant-alpha";
};

static PipelineConfig parse_args(int argc, char* argv[]) {
    PipelineConfig cfg;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            cfg.duration_secs = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--bots") == 0 && i + 1 < argc)
            cfg.num_bots = static_cast<std::size_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--questdb-host") == 0 && i + 1 < argc)
            cfg.questdb_host = argv[++i];
        else if (std::strcmp(argv[i], "--questdb-port") == 0 && i + 1 < argc)
            cfg.questdb_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--redis-host") == 0 && i + 1 < argc)
            cfg.redis_host = argv[++i];
        else if (std::strcmp(argv[i], "--redis-port") == 0 && i + 1 < argc)
            cfg.redis_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--contestant") == 0 && i + 1 < argc)
            cfg.contestant_id = argv[++i];
    }
    return cfg;
}

int main(int argc, char* argv[]) {
    auto cfg = parse_args(argc, argv);

    ::signal(SIGINT, signal_handler);
    ::signal(SIGTERM, signal_handler);
    ::signal(SIGPIPE, SIG_IGN);

    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "--- IICPC END-TO-END PIPELINE TEST ---\n");
    std::fprintf(stderr, "--- Engine → Telemetry → QuestDB + Redis ---\n");

    // Step 1: Connect to QuestDB and Redis
    QuestDBPublisher questdb;
    bool questdb_ok = questdb.connect(cfg.questdb_host, cfg.questdb_port);
    if (!questdb_ok) {
        std::fprintf(stderr, "[pipeline] WARNING: QuestDB not available, skipping persistence\n");
    }

    RedisPublisher redis;
    bool redis_ok = redis.connect(cfg.redis_host, cfg.redis_port);
    if (!redis_ok) {
        std::fprintf(stderr, "[pipeline] WARNING: Redis not available, skipping leaderboard\n");
    }

    // Step 2: Run SHM benchmark
    HugePageArena arena;
    (void)arena.init(512ULL * 1024 * 1024);
    auto tsc_cal = calibrate_tsc();

    using TelemetryRing = SPSCRingBuffer<LatencySample, 1048576>;
    void* ring_mem = arena.allocate_raw(TelemetryRing::TOTAL_SIZE, CACHE_LINE_SIZE);
    TelemetryRing ring(ring_mem, true);

    // SHM channels
    static constexpr std::size_t CHAN_CAP = 65536;
    using ReqChannel  = ShmChannel<ShmRequest, CHAN_CAP>;
    using RespChannel = ShmChannel<ShmResponse, CHAN_CAP>;

    auto* req_buf = static_cast<ShmRequest*>(
        arena.allocate_raw(sizeof(ShmRequest) * CHAN_CAP, CACHE_LINE_SIZE));
    auto* resp_buf = static_cast<ShmResponse*>(
        arena.allocate_raw(sizeof(ShmResponse) * CHAN_CAP, CACHE_LINE_SIZE));

    ReqChannel req_chan;
    RespChannel resp_chan;
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

    // Bot fleet
    BotFleet fleet;
    fleet.init(arena, cfg.num_bots);

    // Engine
    ShmEngine<CHAN_CAP> engine;
    engine.init(req_chan, resp_chan);

    // Telemetry consumer
    TelemetryConsumer consumer;
    ConsumerConfig consumer_config{};
    consumer_config.report_interval_ms = 1000;
    consumer.init(consumer_config, tsc_cal);

    std::thread consumer_thread([&]() { consumer.run(ring); });

    // Step 3: Run benchmark + publish metrics every second
    std::fprintf(stderr, "[pipeline] Running %d-second benchmark with %zu bots...\n\n",
                 cfg.duration_secs, cfg.num_bots);

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(cfg.duration_secs);
    int report_count = 0;

    while (g_running && std::chrono::steady_clock::now() < deadline) {
        engine.run_batch(fleet, ring);

        // Every ~1M iterations, check if we should publish
        auto elapsed = std::chrono::steady_clock::now() - start;
        int current_sec = static_cast<int>(
            std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());

        if (current_sec > report_count) {
            report_count = current_sec;
            auto pct = consumer.latest_percentiles();
            double tps = static_cast<double>(engine.total_recvs()) /
                         std::chrono::duration<double>(elapsed).count();

            // Publish to QuestDB
            if (questdb_ok) {
                auto now_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());

                questdb.publish_latency(
                    cfg.benchmark_id, cfg.contestant_id, now_ns,
                    tps, pct.p50, pct.p90, pct.p99, pct.p999, pct.max,
                    static_cast<uint64_t>(engine.total_recvs()),
                    engine.total_sends() - engine.total_recvs());
            }

            // Publish to Redis leaderboard
            if (redis_ok) {
                double p99_us = static_cast<double>(pct.p99) / 1000.0;
                if (p99_us < 1.0) p99_us = 1.0;
                double score = tps * (1000.0 / p99_us) *
                               (engine.total_sends() == engine.total_recvs() ? 1.0 : 0.5);
                redis.zadd_leaderboard(cfg.contestant_id, score);

                // Store detailed stats
                char key[128];
                std::snprintf(key, sizeof(key), "contestant:%s", cfg.contestant_id);
                char val[64];

                std::snprintf(val, sizeof(val), "%.0f", tps);
                redis.hset(key, "tps", val);

                std::snprintf(val, sizeof(val), "%.1f", static_cast<double>(pct.p50) / 1000.0);
                redis.hset(key, "p50_us", val);

                std::snprintf(val, sizeof(val), "%.1f", static_cast<double>(pct.p99) / 1000.0);
                redis.hset(key, "p99_us", val);

                std::snprintf(val, sizeof(val), "%.2f", score);
                redis.hset(key, "score", val);
            }
        }
    }

    // Step 4: Drain + Shutdown
    const auto end = std::chrono::steady_clock::now();
    const double elapsed_secs = std::chrono::duration<double>(end - start).count();

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

    // Step 5: Final results + publish
    const uint64_t sends = engine.total_sends();
    const uint64_t recvs = engine.total_recvs();
    const double tps = static_cast<double>(recvs) / elapsed_secs;
    const uint64_t drops = (sends > recvs) ? sends - recvs : 0;
    auto pct = consumer.latest_percentiles();

    // Final publish
    if (questdb_ok) {
        auto now_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        questdb.publish_latency(cfg.benchmark_id, cfg.contestant_id, now_ns,
                                tps, pct.p50, pct.p90, pct.p99, pct.p999, pct.max,
                                recvs, drops);
    }

    if (redis_ok) {
        double p99_us = static_cast<double>(pct.p99) / 1000.0;
        if (p99_us < 1.0) p99_us = 1.0;
        double score = tps * (1000.0 / p99_us) * (drops == 0 ? 1.0 : 0.5);
        redis.zadd_leaderboard(cfg.contestant_id, score);
    }

    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "--- PIPELINE E2E RESULTS ---\n");
    std::fprintf(stderr, "--- Duration:     %.2f seconds ---\n", elapsed_secs);
    std::fprintf(stderr, "--- TPS:          %-44.0f ---\n", tps);
    std::fprintf(stderr, "--- Drops:        %-44lu ---\n", drops);
    std::fprintf(stderr, "--- p50:          %10.2f µs ---\n",
                 static_cast<double>(pct.p50) / 1000.0);
    std::fprintf(stderr, "--- p99:          %10.2f µs ---\n",
                 static_cast<double>(pct.p99) / 1000.0);
    std::fprintf(stderr, "--- [%s] QuestDB publish (%lu points) ---\n",
                 questdb_ok ? "✓" : "✗", questdb.total_published());
    std::fprintf(stderr, "--- [%s] Redis leaderboard ---\n",
                 redis_ok ? "✓" : "✗");

    return 0;
}
