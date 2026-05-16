// =============================================================================
// consumer.cpp — Telemetry Consumer Implementation
// =============================================================================
#include "telemetry/consumer.hpp"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <sched.h>
#include <time.h>
#include <x86intrin.h>

namespace iicpc {

void TelemetryConsumer::init(const ConsumerConfig& config,
                              const TscCalibration& tsc_cal) noexcept {
    config_ = config;
    tsc_cal_ = tsc_cal;
    histogram_.init();
    total_samples_ = 0;
    dropped_ = 0;
    last_seq_ = 0;
}

void TelemetryConsumer::run(RingBuffer& ring) noexcept {
    // Pin to CPU if requested
    if (config_.cpu_affinity >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(config_.cpu_affinity, &cpuset);
        if (::sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
            std::fprintf(stderr, "[telemetry] WARNING: Failed to pin to CPU %d: %s\n",
                         config_.cpu_affinity, std::strerror(errno));
        } else {
            std::fprintf(stderr, "[telemetry] Pinned to CPU %d\n", config_.cpu_affinity);
        }
    }

    running_.store(true, std::memory_order_release);

    // Calculate report interval in TSC ticks
    const uint64_t report_ticks = static_cast<uint64_t>(
        static_cast<double>(config_.report_interval_ms) / 1000.0 *
        static_cast<double>(tsc_cal_.tsc_hz));

    uint64_t last_report_tsc = rdtsc();
    uint64_t samples_since_report = 0;

    std::fprintf(stderr, "[telemetry] Consumer started (report every %d ms)\n",
                 config_.report_interval_ms);

    // === HOT LOOP: ZERO ALLOCATIONS ===
    while (running_.load(std::memory_order_acquire)) {
        LatencySample sample;

        // Busy-poll the ring buffer
        if (ring.try_pop(sample)) {
            // Calculate latency in TSC ticks
            const uint64_t latency_ticks = sample.recv_tsc - sample.send_tsc;

            // Convert to nanoseconds and record
            const uint64_t latency_ns = static_cast<uint64_t>(
                static_cast<double>(latency_ticks) * tsc_cal_.tsc_to_ns);
            histogram_.record(latency_ns);

            total_samples_++;
            samples_since_report++;

            // Track max sequence number seen (for final drop calculation)
            if (sample.seq_num > last_seq_) {
                last_seq_ = sample.seq_num;
            }
        } else {
            // Buffer empty — yield with PAUSE to save power on P-cores
            _mm_pause();
        }

        // Periodic reporting
        const uint64_t now_tsc = rdtsc();
        if (now_tsc - last_report_tsc >= report_ticks) {
            latest_pct_ = histogram_.get_percentiles();

            if (latest_pct_.total_count > 0) {
                const double tps = static_cast<double>(samples_since_report) * 1000.0 /
                                   static_cast<double>(config_.report_interval_ms);

                std::fprintf(stderr,
                    "[telemetry] TPS=%.0f  total=%lu  dropped=%lu  "
                    "p50=%.1fus  p90=%.1fus  p99=%.1fus  p999=%.1fus  max=%.1fus\n",
                    tps,
                    latest_pct_.total_count,
                    dropped_,
                    static_cast<double>(latest_pct_.p50) / 1000.0,
                    static_cast<double>(latest_pct_.p90) / 1000.0,
                    static_cast<double>(latest_pct_.p99) / 1000.0,
                    static_cast<double>(latest_pct_.p999) / 1000.0,
                    static_cast<double>(latest_pct_.max) / 1000.0);
            }

            samples_since_report = 0;
            last_report_tsc = now_tsc;
        }
    }

    // Calculate actual drops: (max_seq_seen + 1) - total_samples
    // Only positive if packets were truly lost (not just reordered)
    if (last_seq_ + 1 > total_samples_) {
        dropped_ = last_seq_ + 1 - total_samples_;
    }

    // Final report
    std::fprintf(stderr, "\n[telemetry] === FINAL REPORT ===\n");
    std::fprintf(stderr, "[telemetry] Total samples: %lu\n", total_samples_);
    std::fprintf(stderr, "[telemetry] Max seq seen: %u\n", last_seq_);
    std::fprintf(stderr, "[telemetry] Computed drops: %lu\n", dropped_);
    histogram_.print_summary(1.0); // Already in nanoseconds
}

} // namespace iicpc
