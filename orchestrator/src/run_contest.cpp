// =============================================================================
// run_contest.cpp — Standalone Contest Runner
// =============================================================================
// Usage:
//   ./run_contest --source /path/to/contestant.cpp [options]
//   ./run_contest --system-test --binary /path/to/binary [options]
//
// Options:
//   --source PATH      Contestant source file (for live match)
//   --binary PATH      Skip compilation, use pre-built binary
//   --duration SECS    Match duration (default: 10)
//   --rate OPS         Target orders/sec (default: 100000)
//   --socket PATH      Gateway socket path (default: /tmp/iicpc_contest.sock)
//   --contestant ID    Contestant identifier (default: "test")
//   --no-docker        Compile on host (no Docker)
//   --no-firecracker   Run without Firecracker VM isolation
//   --system-test      Run post-contest system tests (requires --binary)
//   --original-score S Contest score to blend with (default: 1.0)
//   --scenarios all    Run all stress scenarios (default)
// =============================================================================

#include "orchestrator/contest_runner.hpp"

#include <charconv>
#include <cstdio>
#include <cstring>

using namespace iicpc;

// Safe integer parsing — std::atoi is UB on overflow and has no error checking.
static uint32_t safe_parse_u32(const char* str, uint32_t fallback = 0) {
    uint32_t val = fallback;
    std::from_chars(str, str + std::strlen(str), val);
    return val;
}

static double safe_parse_double(const char* str, double fallback = 0.0) {
    // Simple strtod wrapper
    char* end = nullptr;
    double val = std::strtod(str, &end);
    return (end != str) ? val : fallback;
}

int main(int argc, char* argv[]) {
    MatchConfig cfg{};
    cfg.contestant_id = "test";
    cfg.duration_secs = 10;
    cfg.socket_path = "/tmp/iicpc_contest.sock";
    cfg.use_docker_compile = false;
    cfg.use_firecracker = true; // Firecracker MicroVM isolation (default)
    cfg.sdk_include_path = "/home/junior/Desktop/Coding/IICPC/sdk/include";
    cfg.kernel_path = "/home/junior/Desktop/Coding/IICPC/infra/firecracker/vmlinux.bin";
    cfg.rootfs_path = "/home/junior/Desktop/Coding/IICPC/infra/firecracker/base_rootfs.ext4";

    cfg.blaster_cfg.orders_per_batch = 128;
    cfg.blaster_cfg.limit_pct = 60;
    cfg.blaster_cfg.market_pct = 20;
    cfg.blaster_cfg.cancel_pct = 20;

    const char* binary_path = nullptr;
    bool system_test_mode = false;
    double original_score = 1.0;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--source") == 0 && i + 1 < argc)
            cfg.source_path = argv[++i];
        else if (std::strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            cfg.duration_secs = safe_parse_u32(argv[++i], cfg.duration_secs);
        else if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc)
            cfg.orders_per_sec = safe_parse_u32(argv[++i], cfg.orders_per_sec);
        else if (std::strcmp(argv[i], "--socket") == 0 && i + 1 < argc)
            cfg.socket_path = argv[++i];
        else if (std::strcmp(argv[i], "--contestant") == 0 && i + 1 < argc)
            cfg.contestant_id = argv[++i];
        else if (std::strcmp(argv[i], "--no-docker") == 0)
            cfg.use_docker_compile = false;
        else if (std::strcmp(argv[i], "--no-firecracker") == 0)
            cfg.use_firecracker = false;
        else if (std::strcmp(argv[i], "--binary") == 0 && i + 1 < argc)
            binary_path = argv[++i];
        else if (std::strcmp(argv[i], "--system-test") == 0)
            system_test_mode = true;
        else if (std::strcmp(argv[i], "--original-score") == 0 && i + 1 < argc)
            original_score = safe_parse_double(argv[++i], 1.0);
        else if (std::strcmp(argv[i], "--scenarios") == 0 && i + 1 < argc) {
            // Reserved — currently only "all" is supported
            ++i;
        }
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::fprintf(stderr,
                "Usage: %s --source FILE [options]\n"
                "       %s --system-test --binary FILE [options]\n\n"
                "Live match options:\n"
                "  --source FILE       Contestant source (.cpp)\n"
                "  --binary FILE       Pre-built binary (skip compilation)\n"
                "  --duration SECS     Match duration (default: 10)\n"
                "  --rate OPS          Target orders/sec (default: 100000)\n"
                "  --contestant ID     Contestant identifier\n"
                "  --no-firecracker    Run without VM isolation\n\n"
                "System test options:\n"
                "  --system-test       Run post-contest stress tests\n"
                "  --binary FILE       Pre-built binary (REQUIRED for system test)\n"
                "  --original-score S  Contest score to blend with (default: 1.0)\n"
                "  --scenarios all     Run all stress scenarios (default)\n",
                argv[0], argv[0]);
            return 0;
        }
    }

    if (binary_path) {
        cfg.prebuilt_binary = binary_path;
    }

    // =========================================================================
    // System Test Mode
    // =========================================================================
    if (system_test_mode) {
        if (!binary_path) {
            std::fprintf(stderr, "ERROR: --system-test requires --binary\n");
            return 1;
        }

        std::fprintf(stderr, "\n");
        std::fprintf(stderr, "╔══════════════════════════════════════════════════════════╗\n");
        std::fprintf(stderr, "║   IICPC System Test Runner (Post-Contest)               ║\n");
        std::fprintf(stderr, "║   Binary → Stress Scenarios → Validate → Final Score    ║\n");
        std::fprintf(stderr, "╚══════════════════════════════════════════════════════════╝\n");
        std::fprintf(stderr, "\n");
        std::fprintf(stderr, "  Contestant:      %s\n", cfg.contestant_id);
        std::fprintf(stderr, "  Binary:          %s\n", binary_path);
        std::fprintf(stderr, "  Original Score:  %.4f\n", original_score);
        std::fprintf(stderr, "  Isolation:       %s\n",
                     cfg.use_firecracker ? "Firecracker" : "Direct process");
        std::fprintf(stderr, "\n");

        ContestRunner runner;
        SystemTestResult result = runner.run_system_tests(cfg, original_score);

        // Print contestant-facing report (aggregate only)
        result.print_contestant_report();

        return (result.scenarios_passed == result.scenarios_total) ? 0 : 1;
    }

    // =========================================================================
    // Normal Contest Mode
    // =========================================================================
    if (!cfg.source_path && !binary_path) {
        std::fprintf(stderr, "ERROR: --source or --binary required\n");
        return 1;
    }

    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "╔══════════════════════════════════════════════════════════╗\n");
    std::fprintf(stderr, "║   IICPC Contest Runner                                  ║\n");
    std::fprintf(stderr, "║   Compile → Boot → Blast → Validate → Score             ║\n");
    std::fprintf(stderr, "╚══════════════════════════════════════════════════════════╝\n");
    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "  Contestant:   %s\n", cfg.contestant_id);
    if (cfg.source_path)
        std::fprintf(stderr, "  Source:       %s\n", cfg.source_path);
    if (binary_path)
        std::fprintf(stderr, "  Binary:       %s\n", binary_path);
    std::fprintf(stderr, "  Duration:     %u seconds\n", cfg.duration_secs);
    std::fprintf(stderr, "  Socket:       %s\n", cfg.socket_path);
    std::fprintf(stderr, "\n");

    ContestRunner runner;
    MatchResult result = runner.run_match(cfg);

    return result.compiled ? 0 : 1;
}

