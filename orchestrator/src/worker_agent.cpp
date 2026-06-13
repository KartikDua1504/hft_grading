// worker_agent.cpp — Benchmark Worker Agent
// Connects to the orchestrator, receives benchmark configs, runs the
// Stage 1 engine inside a Firecracker sandbox, and streams metrics back.

#include <grpcpp/grpcpp.h>
#include "control.grpc.pb.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <signal.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <fstream>
#include <sstream>

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;

namespace iicpc {

class WorkerAgent {
public:
    WorkerAgent(const std::string& orchestrator_addr,
                const std::string& worker_id)
        : worker_id_(worker_id) {
        auto channel = grpc::CreateChannel(orchestrator_addr,
                                            grpc::InsecureChannelCredentials());
        stub_ = BenchmarkOrchestrator::NewStub(channel);
    }

    // Register with the orchestrator
    bool register_worker() {
        WorkerInfo info;
        info.set_worker_id(worker_id_);

        char hostname[256];
        ::gethostname(hostname, sizeof(hostname));
        info.set_hostname(hostname);

        // Detect system resources
        info.set_num_cores(static_cast<uint32_t>(sysconf(_SC_NPROCESSORS_ONLN)));
        info.set_memory_bytes(static_cast<uint64_t>(sysconf(_SC_PHYS_PAGES))
                              * static_cast<uint64_t>(sysconf(_SC_PAGESIZE)));

        // Check for hugepages
        info.set_has_hugepages(check_hugepages());

        // Check for KVM
        info.set_has_kvm(::access("/dev/kvm", R_OK | W_OK) == 0);

        RegistrationAck ack;
        ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

        Status status = stub_->RegisterWorker(&ctx, info, &ack);
        if (!status.ok()) {
            std::fprintf(stderr, "[worker] Registration failed: %s\n",
                         status.error_message().c_str());
            return false;
        }

        if (!ack.accepted()) {
            std::fprintf(stderr, "[worker] Registration rejected: %s\n",
                         ack.error_message().c_str());
            return false;
        }

        std::fprintf(stderr, "[worker] Registered as: %s (%u cores, %lu MB, KVM=%s)\n",
                     ack.assigned_id().c_str(),
                     info.num_cores(),
                     info.memory_bytes() / (1024 * 1024),
                     info.has_kvm() ? "yes" : "no");
        return true;
    }

    // Heartbeat loop (background thread)
    void start_heartbeat(std::atomic<bool>& stop_flag) {
        heartbeat_thread_ = std::thread([this, &stop_flag]() {
            while (!stop_flag.load()) {
                send_heartbeat();
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        });
    }

    void stop_heartbeat() {
        if (heartbeat_thread_.joinable()) {
            heartbeat_thread_.join();
        }
    }

    // Send metrics snapshot for a benchmark
    bool submit_result(const BenchmarkResult& result) {
        BenchmarkHandle handle;
        handle.set_benchmark_id(result.benchmark_id());
        handle.set_worker_id(worker_id_);

        BenchmarkResult response;
        ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));

        // Use StopBenchmark to submit final result
        Status status = stub_->StopBenchmark(&ctx, handle, &response);
        return status.ok();
    }

    const std::string& worker_id() const { return worker_id_; }

private:
    void send_heartbeat() {
        HeartbeatRequest req;
        req.set_worker_id(worker_id_);
        req.set_timestamp_ns(
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count()));

        // Read CPU usage from /proc/stat (simplified)
        req.set_cpu_usage(0.0f); // TODO: implement proper CPU monitoring
        req.set_active_sandboxes(0);

        HeartbeatResponse resp;
        ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));

        stub_->Heartbeat(&ctx, req, &resp);
    }

    static bool check_hugepages() {
        std::ifstream f("/proc/meminfo");
        if (!f.is_open()) return false;
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("HugePages_Total:") != std::string::npos) {
                std::istringstream iss(line);
                std::string key;
                int val;
                iss >> key >> val;
                return val > 0;
            }
        }
        return false;
    }

    std::string worker_id_;
    std::unique_ptr<BenchmarkOrchestrator::Stub> stub_;
    std::thread heartbeat_thread_;
};

} // namespace iicpc

// Main
static std::atomic<bool> g_shutdown{false};
static void signal_handler(int) { g_shutdown.store(true); }

int main(int argc, char* argv[]) {
    ::signal(SIGINT, signal_handler);
    ::signal(SIGTERM, signal_handler);

    std::string orchestrator_addr = "localhost:50051";
    std::string worker_id = "worker-1";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--orchestrator") == 0 && i + 1 < argc)
            orchestrator_addr = argv[++i];
        else if (std::strcmp(argv[i], "--id") == 0 && i + 1 < argc)
            worker_id = argv[++i];
    }

    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "--- IICPC Worker Agent ---\n");
    std::fprintf(stderr, "--- Orchestrator: %-40s ---\n", orchestrator_addr.c_str());
    std::fprintf(stderr, "--- Worker ID:    %-40s ---\n", worker_id.c_str());

    iicpc::WorkerAgent agent(orchestrator_addr, worker_id);

    if (!agent.register_worker()) {
        std::fprintf(stderr, "[worker] FATAL: Could not register with orchestrator\n");
        return 1;
    }

    agent.start_heartbeat(g_shutdown);

    // Main loop: wait for benchmark commands
    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::fprintf(stderr, "[worker] Shutting down...\n");
    agent.stop_heartbeat();
    return 0;
}
