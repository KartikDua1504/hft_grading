#pragma once
// =============================================================================
// cpu_affinity.hpp — Deterministic CPU Core Pinning
// =============================================================================
// Pin threads to isolated cores for jitter-free benchmarking.
//
// Architecture (on a 48-core c8i.metal-48xl):
//   Cores 0-3:   OS + Docker + NGINX + FastAPI (untouched by isolcpus)
//   Cores 4-7:   Order Blaster (load generator)
//   Cores 8-11:  Contestant process (via taskset)
//   Cores 12-15: Shadow orderbook + validation
//   Cores 16-47: Available for parallel contests
//
// Usage:
//   CpuAffinity::pin_this_thread(4);        // Pin to core 4
//   CpuAffinity::pin_this_thread({4,5,6,7}); // Pin to core set
//   CpuAffinity::pin_pid(pid, 8);           // Pin child process
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sched.h>
#include <pthread.h>
#include <unistd.h>
#include <initializer_list>

namespace iicpc {

// Core assignments (configurable per deployment)
struct CoreMap {
    static constexpr int BLASTER_CORE     = 4;
    static constexpr int BLASTER_CORE_END = 7;
    static constexpr int CONTEST_CORE     = 8;
    static constexpr int CONTEST_CORE_END = 11;
    static constexpr int SHADOW_CORE      = 12;
    static constexpr int SHADOW_CORE_END  = 15;
};

class CpuAffinity {
public:
    // Pin current thread to a single core
    static bool pin_this_thread(int core) noexcept {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core, &cpuset);

        int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
        if (rc != 0) {
            std::fprintf(stderr, "[affinity] Failed to pin thread to core %d: %s\n",
                         core, std::strerror(rc));
            return false;
        }
        std::fprintf(stderr, "[affinity] Thread pinned to core %d\n", core);
        return true;
    }

    // Pin current thread to a set of cores
    static bool pin_this_thread(std::initializer_list<int> cores) noexcept {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        for (int c : cores) CPU_SET(c, &cpuset);

        int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
        if (rc != 0) {
            std::fprintf(stderr, "[affinity] Failed to pin thread to core set: %s\n",
                         std::strerror(rc));
            return false;
        }
        return true;
    }

    // Pin an external process (by PID) to a single core
    static bool pin_pid(pid_t pid, int core) noexcept {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core, &cpuset);

        int rc = sched_setaffinity(pid, sizeof(cpuset), &cpuset);
        if (rc != 0) {
            std::fprintf(stderr, "[affinity] Failed to pin pid %d to core %d: %s\n",
                         pid, core, std::strerror(errno));
            return false;
        }
        std::fprintf(stderr, "[affinity] PID %d pinned to core %d\n", pid, core);
        return true;
    }

    // Pin an external process to a range of cores
    static bool pin_pid_range(pid_t pid, int start, int end) noexcept {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        for (int c = start; c <= end; ++c) CPU_SET(c, &cpuset);

        int rc = sched_setaffinity(pid, sizeof(cpuset), &cpuset);
        if (rc != 0) {
            std::fprintf(stderr, "[affinity] Failed to pin pid %d to cores %d-%d: %s\n",
                         pid, start, end, std::strerror(errno));
            return false;
        }
        std::fprintf(stderr, "[affinity] PID %d pinned to cores %d-%d\n", pid, start, end);
        return true;
    }

    // Set SCHED_FIFO real-time priority (requires root or CAP_SYS_NICE)
    static bool set_realtime(int priority = 90) noexcept {
        struct sched_param param{};
        param.sched_priority = priority;
        int rc = sched_setscheduler(0, SCHED_FIFO, &param);
        if (rc != 0) {
            std::fprintf(stderr, "[affinity] SCHED_FIFO failed (need root): %s\n",
                         std::strerror(errno));
            return false;
        }
        std::fprintf(stderr, "[affinity] SCHED_FIFO priority %d set\n", priority);
        return true;
    }

    // Get current core
    static int current_core() noexcept {
        return sched_getcpu();
    }

    // Print affinity info
    static void print_info() noexcept {
        int ncores = sysconf(_SC_NPROCESSORS_ONLN);
        std::fprintf(stderr, "[affinity] Available cores: %d\n", ncores);
        std::fprintf(stderr, "[affinity] Current core: %d\n", current_core());

        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        sched_getaffinity(0, sizeof(cpuset), &cpuset);

        std::fprintf(stderr, "[affinity] Allowed cores: ");
        for (int i = 0; i < ncores; ++i) {
            if (CPU_ISSET(i, &cpuset)) std::fprintf(stderr, "%d ", i);
        }
        std::fprintf(stderr, "\n");
    }
};

} // namespace iicpc
