// =============================================================================
// run_contest.cpp — Standalone Contest Runner
// =============================================================================
// Usage:
//   ./run_contest --source /path/to/contestant.cpp [options]
//
// Options:
//   --source PATH      Contestant source file (REQUIRED)
//   --duration SECS    Match duration (default: 10)
//   --rate OPS         Target orders/sec (default: 100000)
//   --socket PATH      Gateway socket path (default: /tmp/iicpc_contest.sock)
//   --contestant ID    Contestant identifier (default: "test")
//   --no-docker        Compile on host (no Docker)
//   --binary PATH      Skip compilation, use pre-built binary
// =============================================================================

#include "orchestrator/contest_runner.hpp"

#include <cstdio>
#include <cstring>

using namespace iicpc;

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

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--source") == 0 && i + 1 < argc)
            cfg.source_path = argv[++i];
        else if (std::strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            cfg.duration_secs = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc)
            cfg.orders_per_sec = static_cast<uint32_t>(std::atoi(argv[++i]));
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
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::fprintf(stderr,
                "Usage: %s --source FILE [--duration SECS] [--rate OPS] "
                "[--contestant ID] [--binary PATH] [--no-firecracker]\n", argv[0]);
            return 0;
        }
    }

    if (!cfg.source_path && !binary_path) {
        std::fprintf(stderr, "ERROR: --source or --binary required\n");
        return 1;
    }

    if (binary_path) {
        cfg.prebuilt_binary = binary_path;
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
