#pragma once
// =============================================================================
// post_contest_validator.hpp — CF-Style Post-Contest System Test Harness
// =============================================================================
// After the live contest, this runs every contestant's binary against the
// full stress scenario battery. Each scenario replays a deterministic,
// adversarial order stream and validates correctness via ShadowOrderbook.
//
// Results are aggregated into a single system test score (0.0 - 1.0).
// Contestants see ONLY the aggregate — individual scenario pass/fail is
// hidden (prevents reverse-engineering the test suite between rounds).
//
// Final score blending:
//   final = min(contest_score, 0.6 * contest_score + 0.4 * system_score)
// System tests can only LOWER a ranking, never boost it.
// =============================================================================

#include "exchange/stress_scenarios.hpp"
#include "exchange/shadow_orderbook.hpp"
#include "sandbox/sandbox_bridge.hpp"
#include "sandbox/compiler_service.hpp"
#include "sandbox/firecracker_manager.hpp"
#include "core/arena.hpp"
#include "core/hdr_histogram.hpp"
#include "core/cpu_affinity.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace iicpc {

// =============================================================================
// System Test Aggregate Result
// =============================================================================
struct SystemTestResult {
    char     contestant_id[64] = {};
    uint32_t scenarios_passed  = 0;
    uint32_t scenarios_total   = 0;
    double   system_score      = 0.0;       // Weighted avg (0.0 - 1.0)
    double   original_score    = 0.0;       // From live contest
    double   final_score       = 0.0;       // Blended
    ScenarioResult scenarios[MAX_STRESS_SCENARIOS] = {};

    void print_report() const noexcept {
        std::fprintf(stderr, "\n");
        std::fprintf(stderr, "╔══════════════════════════════════════════════════════════════╗\n");
        std::fprintf(stderr, "║            SYSTEM TEST RESULTS: %-25s   ║\n", contestant_id);
        std::fprintf(stderr, "╠══════════════════════════════════════════════════════════════╣\n");
        std::fprintf(stderr, "║  Scenarios:       %u / %u passed                              ║\n",
                     scenarios_passed, scenarios_total);
        std::fprintf(stderr, "║  System Score:    %-44.4f  ║\n", system_score);
        std::fprintf(stderr, "║  Original Score:  %-44.4f  ║\n", original_score);
        std::fprintf(stderr, "║  Final Score:     %-44.4f  ║\n", final_score);
        std::fprintf(stderr, "╠══════════════════════════════════════════════════════════════╣\n");

        for (uint32_t i = 0; i < scenarios_total; ++i) {
            const auto& sr = scenarios[i];
            std::fprintf(stderr, "║  [%s] Scenario %2u: %-20s  score=%.4f       ║\n",
                         sr.passed ? "✓" : "✗",
                         sr.scenario_id, sr.name, sr.correctness);
        }

        std::fprintf(stderr, "╚══════════════════════════════════════════════════════════════╝\n");
    }

    // Only show aggregate to contestant (no per-scenario breakdown)
    void print_contestant_report() const noexcept {
        std::fprintf(stderr, "\n");
        std::fprintf(stderr, "╔══════════════════════════════════════════════════════════════╗\n");
        std::fprintf(stderr, "║            SYSTEM TEST RESULTS: %-25s   ║\n", contestant_id);
        std::fprintf(stderr, "╠══════════════════════════════════════════════════════════════╣\n");
        std::fprintf(stderr, "║  Scenarios Passed:  %u / %-37u ║\n",
                     scenarios_passed, scenarios_total);
        std::fprintf(stderr, "║  System Score:      %-42.4f║\n", system_score);
        std::fprintf(stderr, "║  Final Score:       %-42.4f║\n", final_score);
        std::fprintf(stderr, "╚══════════════════════════════════════════════════════════════╝\n");
    }
};

// =============================================================================
// Post-Contest Validator
// =============================================================================
class PostContestValidator {
public:
    PostContestValidator() noexcept = default;

    // =========================================================================
    // Run all stress scenarios against a pre-built contestant binary
    // =========================================================================
    SystemTestResult run_system_tests(
        const char* contestant_id,
        const char* binary_path,
        double original_score,
        const char* socket_path = "/tmp/iicpc_systest.sock",
        bool use_firecracker = true,
        const char* kernel_path = nullptr,
        const char* rootfs_path = nullptr) noexcept
    {
        SystemTestResult result{};
        std::strncpy(result.contestant_id, contestant_id,
                     sizeof(result.contestant_id) - 1);
        result.original_score = original_score;

        // Build scenario list
        StressScenario scenarios[MAX_STRESS_SCENARIOS];
        uint32_t num_scenarios = build_stress_scenarios(scenarios);
        result.scenarios_total = num_scenarios;

        std::fprintf(stderr, "\n");
        std::fprintf(stderr, "╔══════════════════════════════════════════════════════════════╗\n");
        std::fprintf(stderr, "║   SYSTEM TEST: %-43s  ║\n", contestant_id);
        std::fprintf(stderr, "║   Running %u stress scenarios with Firecracker isolation     ║\n",
                     num_scenarios);
        std::fprintf(stderr, "╚══════════════════════════════════════════════════════════════╝\n");

        double weighted_sum = 0.0;

        for (uint32_t i = 0; i < num_scenarios; ++i) {
            const auto& scenario = scenarios[i];
            std::fprintf(stderr, "\n[systest] === Scenario %u/%u: %s ===\n",
                         i + 1, num_scenarios, scenario.name);
            std::fprintf(stderr, "[systest]   %s\n", scenario.description);
            std::fprintf(stderr, "[systest]   Duration: %us, Weight: %.2f\n",
                         scenario.duration_secs, scenario.weight);

            ScenarioResult sr = run_single_scenario(
                scenario, binary_path, socket_path,
                use_firecracker, kernel_path, rootfs_path);

            result.scenarios[i] = sr;

            if (sr.passed) {
                result.scenarios_passed++;
            }

            weighted_sum += sr.weighted_score;

            std::fprintf(stderr, "[systest]   → Correctness: %.4f | %s\n",
                         sr.correctness, sr.passed ? "PASSED" : "FAILED");
        }

        // Compute system score
        result.system_score = weighted_sum;

        // Blend: final = min(contest, 0.6 * contest + 0.4 * system_test)
        // System tests can only lower, never boost
        double blended = 0.6 * original_score + 0.4 * result.system_score;
        result.final_score = (blended < original_score) ? blended : original_score;

        return result;
    }

private:
    // =========================================================================
    // Run a single stress scenario
    // =========================================================================
    ScenarioResult run_single_scenario(
        const StressScenario& scenario,
        const char* binary_path,
        const char* socket_path,
        bool use_firecracker,
        const char* kernel_path,
        const char* rootfs_path) noexcept
    {
        ScenarioResult sr{};
        sr.scenario_id = scenario.id;
        std::strncpy(sr.name, scenario.name, sizeof(sr.name) - 1);

        // Initialize arena for this scenario
        HugePageArena arena;
        (void)arena.init(512ULL * 1024 * 1024);
        if (!arena.is_initialized()) {
            std::fprintf(stderr, "[systest]   FATAL: arena init failed\n");
            return sr;
        }

        // Initialize blaster with scenario-specific config
        OrderBlaster blaster;
        if (!blaster.init(arena, scenario.blaster_cfg)) {
            std::fprintf(stderr, "[systest]   FATAL: blaster init failed\n");
            return sr;
        }

        // Initialize shadow orderbook
        ShadowOrderbook shadow;
        if (!shadow.init(arena)) {
            std::fprintf(stderr, "[systest]   FATAL: shadow init failed\n");
            return sr;
        }

        // Initialize bridge
        SandboxBridge bridge;
        if (!bridge.init(arena, socket_path)) {
            std::fprintf(stderr, "[systest]   FATAL: bridge init failed\n");
            return sr;
        }

        // Start contestant process
        pid_t contestant_pid = -1;

        if (use_firecracker && kernel_path && rootfs_path) {
            // Firecracker MicroVM boot
            char rootfs_copy[256];
            std::snprintf(rootfs_copy, sizeof(rootfs_copy),
                          "/tmp/iicpc_systest_%u.ext4", scenario.id);
            char cp_cmd[512];
            std::snprintf(cp_cmd, sizeof(cp_cmd),
                          "cp --reflink=auto '%s' '%s'", rootfs_path, rootfs_copy);
            if (std::system(cp_cmd) != 0) {
                std::fprintf(stderr, "[systest]   FATAL: rootfs copy failed\n");
                return sr;
            }

            // Inject binary
            char inject_cmd[1024];
            std::snprintf(inject_cmd, sizeof(inject_cmd),
                          "debugfs -w -R 'write %s /usr/bin/contestant' '%s' 2>/dev/null",
                          binary_path, rootfs_copy);
            [[maybe_unused]] auto rc = std::system(inject_cmd);

            MicroVMConfig fc_cfg{};
            fc_cfg.kernel_image_path = kernel_path;
            fc_cfg.rootfs_path = rootfs_copy;
            fc_cfg.vcpu_count = 2;
            fc_cfg.mem_size_mib = 256;
            fc_cfg.boot_args = "console=ttyS0 reboot=k panic=1 pci=off root=/dev/vda rw";

            FirecrackerManager fc_mgr;
            if (!fc_mgr.create(fc_cfg) || !fc_mgr.configure() || !fc_mgr.start()) {
                std::fprintf(stderr, "[systest]   FATAL: Firecracker failed: %s\n",
                             fc_mgr.last_error());
                std::remove(rootfs_copy);
                return sr;
            }

            contestant_pid = fc_mgr.pid();

            std::thread accept_thread([&]() {
                (void)bridge.listen_and_accept(15000);
            });
            accept_thread.detach();
            std::this_thread::sleep_for(std::chrono::seconds(3));
        } else {
            // Direct process execution (local testing fallback)
            std::thread accept_thread([&]() {
                (void)bridge.listen_and_accept(10000);
            });

            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            contestant_pid = ::fork();
            if (contestant_pid == 0) {
                ::execl(binary_path, "contestant",
                        "--gateway", socket_path, nullptr);
                ::_exit(127);
            }

            accept_thread.join();
        }

        if (!bridge.is_connected()) {
            std::fprintf(stderr, "[systest]   FATAL: contestant did not connect\n");
            if (contestant_pid > 0) {
                ::kill(contestant_pid, SIGKILL);
                ::waitpid(contestant_pid, nullptr, 0);
            }
            return sr;
        }

        // Send SessionStart
        {
            SessionStart ss{};
            ss.msg_type = MsgType::SESSION_START;
            ss.instrument_count = 1;
            ss.match_duration_ms = scenario.duration_secs * 1000;
            ss.start_timestamp_ns = 0;
            ss.initial_cash = 1000000000;
            ss.max_position = 10000;
            ss.max_order_size = 1000;
            ss.max_orders_per_sec = 200000;
            bridge.send_control(&ss, sizeof(ss));
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        // Blast orders for scenario duration
        const auto match_start = std::chrono::steady_clock::now();
        const auto match_end = match_start +
            std::chrono::seconds(scenario.duration_secs);

        while (std::chrono::steady_clock::now() < match_end) {
            blaster.generate_batch();

            BlastOrder order;
            while (blaster.pop(order)) {
                bridge.send_order(order);

                if (order.type == MsgType::ORDER_ENTRY) {
                    auto* oe = reinterpret_cast<const OrderEntry*>(order.data);
                    shadow.process_order(*oe);
                } else if (order.type == MsgType::CANCEL_REQUEST) {
                    auto* cr = reinterpret_cast<const CancelRequest*>(order.data);
                    shadow.process_cancel(*cr);
                }
            }

            bridge.recv_responses();

            ContestantResponse resp;
            while (bridge.pop_response(resp)) {
                if (resp.response_type == MsgType::FILL) {
                    shadow.validate_fill(resp.order_id, resp.fill_price,
                                         resp.fill_qty);
                } else if (resp.response_type == MsgType::ORDER_ACK) {
                    shadow.result_mut().total_acks++;
                }
            }
        }

        // Send SessionEnd
        {
            SessionEnd se{};
            se.msg_type = MsgType::SESSION_END;
            se.total_orders = static_cast<uint32_t>(bridge.stats().orders_sent);
            bridge.send_control(&se, sizeof(se));
        }

        // Drain final responses
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
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

        // Shutdown
        bridge.shutdown();
        if (contestant_pid > 0) {
            ::kill(contestant_pid, SIGTERM);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            ::kill(contestant_pid, SIGKILL);
            ::waitpid(contestant_pid, nullptr, 0);
        }

        shadow.finalize();

        // Populate scenario result
        const auto& vr = shadow.result();
        sr.correctness     = vr.correctness_score();
        sr.orders_sent     = bridge.stats().orders_sent;
        sr.responses_recv  = bridge.stats().responses_recv;
        sr.correct_fills   = vr.correct_fills;
        sr.wrong_fills     = vr.wrong_price + vr.wrong_qty;
        sr.missing_fills   = vr.missing_fills;
        sr.extra_fills     = vr.extra_fills;
        sr.priority_errors = vr.priority_errors;
        sr.weighted_score  = sr.correctness * scenario.weight;

        // Pass threshold: ≥ 0.8 correctness
        sr.passed = (sr.correctness >= 0.8);

        return sr;
    }
};

} // namespace iicpc
