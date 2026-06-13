#pragma once
// contest_runner.hpp — Full End-to-End Match Orchestrator
// Orchestrates the full contest pipeline:
//
//   1. Compile contestant code → static binary
//   2. Inject binary into Firecracker rootfs
//   3. Boot Firecracker microVM
//   4. Start SandboxBridge (UDS host↔VM)
//   5. Initialize OrderBlaster + ShadowOrderbook
//   6. Blast orders → measure latency → validate correctness
//   7. Stop VM → compute final score → update leaderboard
//
// Scoring formula:
//   Score = PnL_weight * correctness_score * throughput_score * latency_score
//   Where:
//     correctness_score = shadow_validation (0.0 - 1.0)
//     throughput_score  = orders_processed / orders_sent
//     latency_score     = 1000.0 / p99_latency_us
//     PnL_weight        = 1.0 (PnL is PRIMARY metric)

#include "loadgen/order_blaster.hpp"
#include "exchange/shadow_orderbook.hpp"
#include "exchange/stress_scenarios.hpp"
#include "orchestrator/post_contest_validator.hpp"
#include "sandbox/sandbox_bridge.hpp"
#include "sandbox/compiler_service.hpp"
#include "sandbox/firecracker_manager.hpp"
#include "core/arena.hpp"
#include "core/hot_path_asm.hpp"
#include "core/cpu_affinity.hpp"
#include "core/hdr_histogram.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace iicpc {

// Match Configuration
struct MatchConfig {
    // Contestant
    const char* contestant_id    = "unknown";
    const char* source_path      = nullptr;  // Path to .cpp file
    const char* prebuilt_binary  = nullptr;  // Skip compilation, use this binary

    // Match parameters
    uint32_t duration_secs       = 30;
    uint32_t orders_per_sec      = 100000;   // Target blast rate

    // Blaster config
    OrderBlasterConfig blaster_cfg{};

    // Paths
    const char* sdk_include_path = nullptr;
    const char* rootfs_path      = nullptr;
    const char* kernel_path      = nullptr;
    const char* socket_path      = "/tmp/iicpc_contest.sock";

    // Scoring weights
    double correctness_weight    = 0.4;
    double throughput_weight     = 0.3;
    double latency_weight        = 0.3;

    // Sandbox
    bool use_firecracker         = true;
    bool use_docker_compile      = false;
};

// Match Result
struct MatchResult {
    // Identity
    char contestant_id[64] = {};

    // Compilation
    bool   compiled          = false;
    double compile_time_secs = 0.0;

    // Performance
    uint64_t orders_sent     = 0;
    uint64_t responses_recv  = 0;
    uint64_t drops           = 0;
    double   throughput      = 0.0;  // Orders/sec processed by contestant

    // Latency (nanoseconds)
    uint64_t p50_ns          = 0;
    uint64_t p99_ns          = 0;
    uint64_t p999_ns         = 0;
    uint64_t max_ns          = 0;

    // Correctness
    double   correctness     = 0.0;

    // Final score
    double   score           = 0.0;

    // Error (if any)
    const char* error_msg    = nullptr;

    void print() const noexcept {
        std::fprintf(stderr, "\n");
        std::fprintf(stderr, "--- MATCH RESULT: %-25s ---\n", contestant_id);
        std::fprintf(stderr, "--- Compiled:      %s (%.2fs) ---\n",
                     compiled ? "YES" : "NO ", compile_time_secs);
        std::fprintf(stderr, "--- Orders Sent:   %-44lu ---\n", orders_sent);
        std::fprintf(stderr, "--- Responses:     %-44lu ---\n", responses_recv);
        std::fprintf(stderr, "--- Drops:         %-44lu ---\n", drops);
        std::fprintf(stderr, "--- Throughput:    %-40.0f OPS ---\n", throughput);
        std::fprintf(stderr, "--- p50 Latency:   %-40lu ns ---\n", p50_ns);
        std::fprintf(stderr, "--- p99 Latency:   %-40lu ns ---\n", p99_ns);
        std::fprintf(stderr, "--- Correctness:   %-40.4f ---\n", correctness);
        std::fprintf(stderr, "--- ★ FINAL SCORE: %-43.2f ---\n", score);
    }
};

// Contest Runner
class ContestRunner {
public:
    ContestRunner() noexcept = default;

    MatchResult run_match(const MatchConfig& cfg) noexcept {
        MatchResult result{};
        std::strncpy(result.contestant_id, cfg.contestant_id,
                     sizeof(result.contestant_id) - 1);

        std::fprintf(stderr, "\n[runner] === Starting match for '%s' ===\n",
                     cfg.contestant_id);

        // 1. Compile (or use pre-built binary)
        const char* binary_path = nullptr;

        if (cfg.prebuilt_binary) {
            std::fprintf(stderr, "[runner] [1/6] Using pre-built binary: %s\n",
                         cfg.prebuilt_binary);
            binary_path = cfg.prebuilt_binary;
            result.compiled = true;
        } else {
            std::fprintf(stderr, "[runner] [1/6] Compiling contestant code...\n");

            CompileConfig cc{};
            cc.source_path = cfg.source_path;
            cc.output_path = "/tmp/iicpc_contestant_bin";
            cc.sdk_include = cfg.sdk_include_path;
            cc.use_docker = cfg.use_docker_compile;
            cc.timeout_secs = 30;

            CompileResult cr = CompilerService::compile(cc);
            result.compiled = cr.success;
            result.compile_time_secs = cr.compile_secs;

            if (!cr.success) {
                std::fprintf(stderr, "[runner] Compilation FAILED:\n%s\n", cr.error);
                result.score = 0.0;
                return result;
            }
            std::fprintf(stderr, "[runner]   → Compiled in %.2fs\n", cr.compile_secs);
            binary_path = cc.output_path;
        }

        // 2. Initialize arena + components
        std::fprintf(stderr, "[runner] [2/6] Initializing arena + components...\n");

        HugePageArena arena;
        (void)arena.init(512ULL * 1024 * 1024);
        if (!arena.is_initialized()) {
            std::fprintf(stderr, "[runner] FATAL: arena init failed\n");
            return result;
        }

        OrderBlaster blaster;
        if (!blaster.init(arena, cfg.blaster_cfg)) {
            std::fprintf(stderr, "[runner] FATAL: blaster init failed\n");
            return result;
        }

        ShadowOrderbook shadow;
        if (!shadow.init(arena)) {
            std::fprintf(stderr, "[runner] FATAL: shadow init failed\n");
            return result;
        }

        SandboxBridge bridge;
        if (!bridge.init(arena, cfg.socket_path)) {
            std::fprintf(stderr, "[runner] FATAL: bridge init failed\n");
            return result;
        }

        // 3. Boot VM / Start process + CPU Pinning
        std::fprintf(stderr, "[runner] [3/6] Starting contestant process...\n");

        // Pin the blaster/orchestrator thread to isolated core
        CpuAffinity::pin_this_thread(CoreMap::BLASTER_CORE);
        std::fprintf(stderr, "[runner]   → Orchestrator pinned to core %d\n",
                     CoreMap::BLASTER_CORE);

        pid_t contestant_pid = -1;

        if (cfg.use_firecracker && cfg.kernel_path && cfg.rootfs_path) {
            // Firecracker MicroVM boot + binary injection
            std::fprintf(stderr, "[runner]   → Firecracker MicroVM isolation\n");

            // Inject contestant binary into rootfs copy BEFORE boot
            char rootfs_copy[256];
            std::snprintf(rootfs_copy, sizeof(rootfs_copy),
                          "/tmp/iicpc_rootfs_%s.ext4", cfg.contestant_id);
            char cp_cmd[512];
            std::snprintf(cp_cmd, sizeof(cp_cmd),
                          "cp --reflink=auto '%s' '%s'", cfg.rootfs_path, rootfs_copy);
            if (std::system(cp_cmd) != 0) {
                std::fprintf(stderr, "[runner] FATAL: rootfs copy failed\n");
                result.error_msg = "rootfs copy failed";
                return result;
            }

            // Inject via debugfs (rootless)
            std::fprintf(stderr, "[runner]   → Injecting binary into rootfs copy\n");
            char inject_cmd[1024];
            std::snprintf(inject_cmd, sizeof(inject_cmd),
                          "debugfs -w -R 'write %s /usr/bin/contestant' '%s' 2>/dev/null",
                          binary_path, rootfs_copy);
            [[maybe_unused]] auto rc = std::system(inject_cmd);

            // Configure and boot Firecracker
            MicroVMConfig fc_cfg{};
            fc_cfg.kernel_image_path = cfg.kernel_path;
            fc_cfg.rootfs_path = rootfs_copy;
            fc_cfg.vcpu_count = 2;
            fc_cfg.mem_size_mib = 256;
            fc_cfg.boot_args = "console=ttyS0 reboot=k panic=1 pci=off root=/dev/vda rw";

            FirecrackerManager fc_mgr;
            if (!fc_mgr.create(fc_cfg)) {
                std::fprintf(stderr, "[runner] FATAL: FC create failed: %s\n",
                             fc_mgr.last_error());
                result.error_msg = "Firecracker create failed";
                std::remove(rootfs_copy);
                return result;
            }
            if (!fc_mgr.configure()) {
                std::fprintf(stderr, "[runner] FATAL: FC configure failed: %s\n",
                             fc_mgr.last_error());
                result.error_msg = "Firecracker configure failed";
                std::remove(rootfs_copy);
                return result;
            }
            if (!fc_mgr.start()) {
                std::fprintf(stderr, "[runner] FATAL: FC start failed: %s\n",
                             fc_mgr.last_error());
                result.error_msg = "Firecracker start failed";
                std::remove(rootfs_copy);
                return result;
            }

            std::fprintf(stderr, "[runner]   → VM booted (pid=%d)\n", fc_mgr.pid());
            contestant_pid = fc_mgr.pid();

            // The contestant init inside the VM will connect to the UDS
            std::thread accept_thread([&]() {
                (void)bridge.listen_and_accept(15000);
            });
            accept_thread.detach();
            std::this_thread::sleep_for(std::chrono::seconds(3)); // Wait for VM boot
        } else {
            // Direct process execution (for local testing)
            // Start bridge listener first
            std::thread accept_thread([&]() {
                (void)bridge.listen_and_accept(10000);
            });

            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            contestant_pid = ::fork();
            if (contestant_pid == 0) {
                ::execl(binary_path, "contestant",
                        "--gateway", cfg.socket_path, nullptr);
                ::_exit(127);
            }

            // Pin contestant process to isolated cores 8-11
            CpuAffinity::pin_pid_range(
                contestant_pid, CoreMap::CONTEST_CORE, CoreMap::CONTEST_CORE_END);
            std::fprintf(stderr, "[runner]   → Contestant pinned to cores %d-%d\n",
                         CoreMap::CONTEST_CORE, CoreMap::CONTEST_CORE_END);

            std::fprintf(stderr, "[runner]   → Process started (pid=%d)\n",
                         contestant_pid);
            accept_thread.join();
        }

        if (!bridge.is_connected()) {
            std::fprintf(stderr, "[runner] FATAL: contestant did not connect\n");
            if (contestant_pid > 0) {
                ::kill(contestant_pid, SIGKILL);
                ::waitpid(contestant_pid, nullptr, 0);
            }
            return result;
        }

        // 4. Blast orders
        std::fprintf(stderr, "[runner] [4/6] Blasting orders for %us...\n",
                     cfg.duration_secs);

        // Send SessionStart to contestant
        {
            SessionStart session_start{};
            session_start.msg_type = MsgType::SESSION_START;
            session_start.instrument_count = 1;
            session_start.match_duration_ms = cfg.duration_secs * 1000;
            session_start.start_timestamp_ns = 0;
            session_start.initial_cash = 1000000000; // $100K scaled
            session_start.max_position = 10000;
            session_start.max_order_size = 1000;
            session_start.max_orders_per_sec = cfg.orders_per_sec;
            bridge.send_control(&session_start, sizeof(session_start));
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        const auto match_start = std::chrono::steady_clock::now();
        const auto match_end = match_start +
            std::chrono::seconds(cfg.duration_secs);

        int report_interval = 0;

        while (std::chrono::steady_clock::now() < match_end) {
            // Generate orders
            blaster.generate_batch();

            // Send to contestant via bridge
            BlastOrder order;
            while (blaster.pop(order)) {
                bridge.send_order(order);

                // Feed shadow orderbook (same order stream)
                if (order.type == MsgType::ORDER_ENTRY) {
                    auto* oe = reinterpret_cast<const OrderEntry*>(order.data);
                    shadow.process_order(*oe);
                } else if (order.type == MsgType::CANCEL_REQUEST) {
                    auto* cr2 = reinterpret_cast<const CancelRequest*>(order.data);
                    shadow.process_cancel(*cr2);
                }
            }

            // Receive responses
            bridge.recv_responses();

            // Validate responses against shadow
            // CRITICAL: Only validate FILL responses. ACKs are acknowledgements
            // and must NOT be counted as fill attempts.
            ContestantResponse resp;
            while (bridge.pop_response(resp)) {
                if (resp.response_type == MsgType::FILL) {
                    shadow.validate_fill(resp.order_id, resp.fill_price,
                                         resp.fill_qty);
                } else if (resp.response_type == MsgType::ORDER_ACK) {
                    // ACK — count but don't validate as fill
                    shadow.result_mut().total_acks++;
                }
            }

            // Periodic report
            auto elapsed = std::chrono::steady_clock::now() - match_start;
            int secs = static_cast<int>(
                std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
            if (secs > report_interval) {
                report_interval = secs;
                auto& bs = bridge.stats();
                std::fprintf(stderr,
                    "[runner] t=%ds | sent=%lu recv=%lu drops=%lu\n",
                    secs, bs.orders_sent, bs.responses_recv, bs.drops);
            }
        }

        // 5. Stop + collect
        std::fprintf(stderr, "[runner] [5/6] Stopping and collecting results...\n");

        // Send SessionEnd to contestant
        {
            SessionEnd session_end{};
            session_end.msg_type = MsgType::SESSION_END;
            session_end.total_orders = static_cast<uint32_t>(bridge.stats().orders_sent);
            bridge.send_control(&session_end, sizeof(session_end));
        }

        // Give contestant time to flush final responses
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Drain remaining responses
        bridge.recv_responses();
        ContestantResponse final_resp;
        while (bridge.pop_response(final_resp)) {
            if (final_resp.response_type == MsgType::FILL) {
                shadow.validate_fill(final_resp.order_id, final_resp.fill_price,
                                     final_resp.fill_qty);
            } else if (final_resp.response_type == MsgType::ORDER_ACK) {
                shadow.result_mut().total_acks++;
            }
        }

        bridge.shutdown();
        if (contestant_pid > 0) {
            ::kill(contestant_pid, SIGTERM);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            ::kill(contestant_pid, SIGKILL);
            ::waitpid(contestant_pid, nullptr, 0);
        }

        shadow.finalize();

        // 6. Score
        std::fprintf(stderr, "[runner] [6/6] Computing score...\n");

        auto& bs = bridge.stats();
        auto& vr = shadow.result();
        double elapsed_secs = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - match_start).count();

        result.orders_sent = bs.orders_sent;
        result.responses_recv = bs.responses_recv;
        result.drops = bs.drops;
        result.throughput = static_cast<double>(bs.responses_recv) / elapsed_secs;
        result.correctness = vr.correctness_score();

        // Throughput score: fraction processed
        double throughput_score = (bs.orders_sent > 0)
            ? static_cast<double>(bs.responses_recv) /
              static_cast<double>(bs.orders_sent)
            : 0.0;

        // Latency scoring from HDR histogram
        HdrHistogram lat_hist;
        lat_hist.init();
        OrderLatency olat{};
        while (bridge.pop_latency(olat)) {
            if (!olat.was_dropped && olat.latency_ns > 0) {
                lat_hist.record(olat.latency_ns);
            }
        }

        auto lat_pcts = lat_hist.get_percentiles();
        result.p50_ns  = lat_pcts.p50;
        result.p99_ns  = lat_pcts.p99;
        result.p999_ns = lat_pcts.p999;
        result.max_ns  = lat_pcts.max;

        // Latency score: 1.0 for p99 ≤ 1µs, 0.0 for p99 ≥ 100µs
        // Linear interpolation in log space
        double p99_us = static_cast<double>(result.p99_ns) / 1000.0;
        double latency_score;
        if (p99_us <= 1.0)        latency_score = 1.0;
        else if (p99_us >= 100.0) latency_score = 0.0;
        else                      latency_score = 1.0 - (std::log10(p99_us) / 2.0);

        result.score = cfg.correctness_weight * result.correctness +
                       cfg.throughput_weight * throughput_score +
                       cfg.latency_weight * latency_score;

        // Print reports
        shadow.print_report();
        bridge.print_stats();
        result.print();

        return result;
    }

    // Run post-contest system tests against a pre-built binary
    SystemTestResult run_system_tests(const MatchConfig& cfg,
                                       double original_score) noexcept {
        const char* binary_path = cfg.prebuilt_binary;
        if (!binary_path) {
            std::fprintf(stderr, "[runner] System test requires --binary\n");
            SystemTestResult empty{};
            return empty;
        }

        // Use a separate socket for system tests to avoid conflicts
        const char* systest_socket = "/tmp/iicpc_systest.sock";

        PostContestValidator validator;
        SystemTestResult result = validator.run_system_tests(
            cfg.contestant_id,
            binary_path,
            original_score,
            systest_socket,
            cfg.use_firecracker,
            cfg.kernel_path,
            cfg.rootfs_path);

        // Print full report (admin-side, shows per-scenario breakdown)
        result.print_report();

        return result;
    }
};

} // namespace iicpc

