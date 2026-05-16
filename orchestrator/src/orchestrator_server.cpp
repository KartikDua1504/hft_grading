// =============================================================================
// orchestrator_server.cpp — Async gRPC Benchmark Orchestrator
// =============================================================================
// Central control plane for managing benchmark workers, contestant sandboxes,
// and leaderboard aggregation.
//
// Features:
//   - Worker registration with heartbeat monitoring
//   - Benchmark lifecycle: create → start → stream metrics → stop → score
//   - Server-side streaming for real-time metrics
//   - Leaderboard aggregation with scoring
// =============================================================================

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include "control.grpc.pb.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <signal.h>
#include <string>
#include <thread>
#include <vector>

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerWriter;
using grpc::Status;

namespace iicpc {

// =============================================================================
// Worker Registry
// =============================================================================
struct WorkerState {
    std::string worker_id;
    std::string hostname;
    uint32_t num_cores = 0;
    uint64_t memory_bytes = 0;
    bool has_hugepages = false;
    bool has_kvm = false;
    std::chrono::steady_clock::time_point last_heartbeat;
    float cpu_usage = 0.0f;
    uint32_t active_sandboxes = 0;
    bool alive = true;
};

struct BenchmarkState {
    std::string benchmark_id;
    std::string contestant_id;
    std::string worker_id;
    BenchmarkStatus::State state = BenchmarkStatus::PENDING;
    std::chrono::steady_clock::time_point start_time;
    BenchmarkResult result;
    std::vector<MetricsSnapshot> metrics_history;
};

struct LeaderboardEntry_internal {
    std::string contestant_id;
    double score = 0.0;
    double achieved_tps = 0.0;
    LatencyProfile latency;
    uint64_t drops = 0;
};

// =============================================================================
// Orchestrator Service Implementation
// =============================================================================
class OrchestratorServiceImpl final : public BenchmarkOrchestrator::Service {
public:
    // =========================================================================
    // Worker Registration
    // =========================================================================
    Status RegisterWorker(ServerContext* /*ctx*/,
                          const WorkerInfo* request,
                          RegistrationAck* response) override {
        std::lock_guard<std::mutex> lock(mu_);

        WorkerState ws;
        ws.worker_id = request->worker_id();
        ws.hostname = request->hostname();
        ws.num_cores = request->num_cores();
        ws.memory_bytes = request->memory_bytes();
        ws.has_hugepages = request->has_hugepages();
        ws.has_kvm = request->has_kvm();
        ws.last_heartbeat = std::chrono::steady_clock::now();
        ws.alive = true;

        workers_[ws.worker_id] = ws;

        response->set_accepted(true);
        response->set_assigned_id(ws.worker_id);

        std::fprintf(stderr, "[orchestrator] Worker registered: %s (%s, %u cores, %lu MB)\n",
                     ws.worker_id.c_str(), ws.hostname.c_str(),
                     ws.num_cores, ws.memory_bytes / (1024 * 1024));
        return Status::OK;
    }

    // =========================================================================
    // Heartbeat
    // =========================================================================
    Status Heartbeat(ServerContext* /*ctx*/,
                     const HeartbeatRequest* request,
                     HeartbeatResponse* response) override {
        std::lock_guard<std::mutex> lock(mu_);

        auto it = workers_.find(request->worker_id());
        if (it != workers_.end()) {
            it->second.last_heartbeat = std::chrono::steady_clock::now();
            it->second.cpu_usage = request->cpu_usage();
            it->second.active_sandboxes = request->active_sandboxes();
            it->second.alive = true;
        }

        response->set_ok(true);
        return Status::OK;
    }

    // =========================================================================
    // Start Benchmark
    // =========================================================================
    Status StartBenchmark(ServerContext* /*ctx*/,
                          const BenchmarkConfig* request,
                          BenchmarkHandle* response) override {
        std::lock_guard<std::mutex> lock(mu_);

        // Find an available worker
        std::string worker_id;
        for (auto& [id, ws] : workers_) {
            if (ws.alive) {
                worker_id = id;
                break;
            }
        }

        if (worker_id.empty()) {
            return Status(grpc::StatusCode::UNAVAILABLE, "No workers available");
        }

        BenchmarkState bs;
        bs.benchmark_id = request->benchmark_id();
        bs.contestant_id = request->contestant_id();
        bs.worker_id = worker_id;
        bs.state = BenchmarkStatus::RUNNING;
        bs.start_time = std::chrono::steady_clock::now();

        benchmarks_[bs.benchmark_id] = bs;

        response->set_benchmark_id(bs.benchmark_id);
        response->set_worker_id(worker_id);

        std::fprintf(stderr, "[orchestrator] Benchmark started: %s (contestant=%s, worker=%s)\n",
                     bs.benchmark_id.c_str(), bs.contestant_id.c_str(), worker_id.c_str());
        return Status::OK;
    }

    // =========================================================================
    // Stop Benchmark
    // =========================================================================
    Status StopBenchmark(ServerContext* /*ctx*/,
                         const BenchmarkHandle* request,
                         BenchmarkResult* response) override {
        std::lock_guard<std::mutex> lock(mu_);

        auto it = benchmarks_.find(request->benchmark_id());
        if (it == benchmarks_.end()) {
            return Status(grpc::StatusCode::NOT_FOUND, "Benchmark not found");
        }

        auto& bs = it->second;
        bs.state = BenchmarkStatus::COMPLETED;

        auto elapsed = std::chrono::steady_clock::now() - bs.start_time;
        double elapsed_secs = std::chrono::duration<double>(elapsed).count();

        response->set_benchmark_id(bs.benchmark_id);
        response->set_contestant_id(bs.contestant_id);
        response->set_elapsed_secs(elapsed_secs);

        // Copy result if we have one from metrics
        *response = bs.result;
        response->set_benchmark_id(bs.benchmark_id);
        response->set_contestant_id(bs.contestant_id);
        response->set_elapsed_secs(elapsed_secs);

        // Update leaderboard
        update_leaderboard(bs.contestant_id, *response);

        std::fprintf(stderr, "[orchestrator] Benchmark stopped: %s (%.2fs)\n",
                     bs.benchmark_id.c_str(), elapsed_secs);
        return Status::OK;
    }

    // =========================================================================
    // Get Benchmark Status
    // =========================================================================
    Status GetBenchmarkStatus(ServerContext* /*ctx*/,
                              const BenchmarkHandle* request,
                              BenchmarkStatus* response) override {
        std::lock_guard<std::mutex> lock(mu_);

        auto it = benchmarks_.find(request->benchmark_id());
        if (it == benchmarks_.end()) {
            return Status(grpc::StatusCode::NOT_FOUND, "Benchmark not found");
        }

        auto& bs = it->second;
        response->set_state(bs.state);
        response->set_benchmark_id(bs.benchmark_id);

        auto elapsed = std::chrono::steady_clock::now() - bs.start_time;
        response->set_elapsed_secs(std::chrono::duration<double>(elapsed).count());

        return Status::OK;
    }

    // =========================================================================
    // Stream Metrics (server-side streaming)
    // =========================================================================
    Status StreamMetrics(ServerContext* ctx,
                         const BenchmarkHandle* request,
                         ServerWriter<MetricsSnapshot>* writer) override {
        const std::string bench_id = request->benchmark_id();

        while (!ctx->IsCancelled()) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                auto it = benchmarks_.find(bench_id);
                if (it == benchmarks_.end() ||
                    it->second.state == BenchmarkStatus::COMPLETED ||
                    it->second.state == BenchmarkStatus::FAILED) {
                    break;
                }

                if (!it->second.metrics_history.empty()) {
                    writer->Write(it->second.metrics_history.back());
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        return Status::OK;
    }

    // =========================================================================
    // Leaderboard
    // =========================================================================
    Status GetLeaderboard(ServerContext* /*ctx*/,
                          const LeaderboardRequest* request,
                          Leaderboard* response) override {
        std::lock_guard<std::mutex> lock(mu_);

        // Sort leaderboard by score (descending)
        std::vector<std::pair<std::string, LeaderboardEntry_internal>> sorted(
            leaderboard_.begin(), leaderboard_.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) {
                      return a.second.score > b.second.score;
                  });

        uint32_t top_n = request->top_n();
        if (top_n == 0) top_n = static_cast<uint32_t>(sorted.size());

        for (uint32_t i = 0; i < std::min(top_n, static_cast<uint32_t>(sorted.size())); ++i) {
            auto* entry = response->add_entries();
            entry->set_rank(i + 1);
            entry->set_contestant_id(sorted[i].second.contestant_id);
            entry->set_score(sorted[i].second.score);
            entry->set_achieved_tps(sorted[i].second.achieved_tps);
            *entry->mutable_latency() = sorted[i].second.latency;
            entry->set_drops(sorted[i].second.drops);
        }

        return Status::OK;
    }

    // =========================================================================
    // Ingest metrics from a worker (called internally, not via RPC)
    // =========================================================================
    void ingest_metrics(const std::string& benchmark_id,
                        const MetricsSnapshot& snapshot) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = benchmarks_.find(benchmark_id);
        if (it != benchmarks_.end()) {
            it->second.metrics_history.push_back(snapshot);
        }
    }

    void submit_result(const std::string& benchmark_id,
                       const BenchmarkResult& result) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = benchmarks_.find(benchmark_id);
        if (it != benchmarks_.end()) {
            it->second.result = result;
            it->second.state = BenchmarkStatus::COMPLETED;
        }
    }

private:
    /// Scoring: higher TPS + lower latency = better score
    void update_leaderboard(const std::string& contestant_id,
                            const BenchmarkResult& result) {
        LeaderboardEntry_internal entry;
        entry.contestant_id = contestant_id;
        entry.achieved_tps = result.achieved_tps();
        entry.drops = result.drops();
        if (result.has_latency()) {
            entry.latency = result.latency();
        }

        // Score formula: TPS * (1M / p99_ns) * (drops == 0 ? 1.0 : 0.5)
        double p99_us = result.has_latency()
            ? static_cast<double>(result.latency().p99_ns()) / 1000.0
            : 1000.0;
        if (p99_us < 1.0) p99_us = 1.0;

        double drop_penalty = (result.drops() == 0) ? 1.0 : 0.5;
        entry.score = result.achieved_tps() * (1000.0 / p99_us) * drop_penalty;

        leaderboard_[contestant_id] = entry;
    }

    std::mutex mu_;
    std::map<std::string, WorkerState> workers_;
    std::map<std::string, BenchmarkState> benchmarks_;
    std::map<std::string, LeaderboardEntry_internal> leaderboard_;
};

} // namespace iicpc

// =============================================================================
// Main
// =============================================================================
static std::atomic<bool> g_shutdown{false};
static void signal_handler(int) { g_shutdown.store(true); }

int main(int argc, char* argv[]) {
    ::signal(SIGINT, signal_handler);
    ::signal(SIGTERM, signal_handler);

    std::string listen_addr = "0.0.0.0:50051";
    if (argc > 1) listen_addr = argv[1];

    grpc::EnableDefaultHealthCheckService(true);

    iicpc::OrchestratorServiceImpl service;

    ServerBuilder builder;
    builder.AddListeningPort(listen_addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    auto server = builder.BuildAndStart();
    if (!server) {
        std::fprintf(stderr, "[orchestrator] FATAL: Failed to start server on %s\n",
                     listen_addr.c_str());
        return 1;
    }

    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "╔══════════════════════════════════════════════════════════╗\n");
    std::fprintf(stderr, "║   IICPC Benchmark Orchestrator                          ║\n");
    std::fprintf(stderr, "║   Listening on: %-40s  ║\n", listen_addr.c_str());
    std::fprintf(stderr, "╚══════════════════════════════════════════════════════════╝\n\n");

    // Wait for shutdown
    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::fprintf(stderr, "[orchestrator] Shutting down...\n");
    server->Shutdown();
    return 0;
}
