#pragma once
// =============================================================================
// consumer.hpp — Dedicated Telemetry Consumer Thread
// =============================================================================
#include "core/types.hpp"
#include "core/ring_buffer.hpp"
#include "core/hdr_histogram.hpp"
#include "core/tsc.hpp"
#include <atomic>
#include <cstdint>

namespace iicpc {

/// Telemetry consumer configuration
struct ConsumerConfig {
    int cpu_affinity = -1;       // Core to pin to
    int report_interval_ms = 1000; // How often to dump metrics
};

/// The telemetry consumer. Runs on its own pinned thread.
/// Busy-polls the SPSC ring buffer, feeds latency into HDR histogram,
/// periodically dumps p50/p90/p99 to stderr.
class TelemetryConsumer {
public:
    using RingBuffer = SPSCRingBuffer<LatencySample, 1048576>; // 1M slots

    TelemetryConsumer() noexcept = default;

    /// Initialize. Must be called before run().
    void init(const ConsumerConfig& config, const TscCalibration& tsc_cal) noexcept;

    /// Run the consumer loop. Blocks until stop() is called.
    /// This function should be launched on its own thread.
    void run(RingBuffer& ring) noexcept;

    /// Signal the consumer to stop.
    void stop() noexcept { running_.store(false, std::memory_order_release); }

    /// Get the latest snapshot of metrics
    [[nodiscard]] HdrHistogram::Percentiles latest_percentiles() const noexcept {
        return latest_pct_;
    }

    [[nodiscard]] uint64_t total_samples() const noexcept { return total_samples_; }
    [[nodiscard]] uint64_t dropped_samples() const noexcept { return dropped_; }

private:
    ConsumerConfig config_{};
    TscCalibration tsc_cal_{};
    HdrHistogram histogram_;
    HdrHistogram::Percentiles latest_pct_{};

    std::atomic<bool> running_{false};
    uint64_t total_samples_ = 0;
    uint64_t dropped_ = 0;
    uint32_t last_seq_ = 0;
};

} // namespace iicpc
