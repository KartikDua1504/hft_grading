#pragma once

// --- Bounded-Memory HDR Histogram for Latency Percentiles ---
#include "core/types.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace iicpc {

struct HdrHistConfig {
    uint64_t lowest_trackable = 1;
    uint64_t highest_trackable = 10'000'000'000ULL;
    int significant_figures = 2;
};

class HdrHistogram {
public:
    static constexpr std::size_t MAX_BUCKETS = 4096;

    HdrHistogram() noexcept = default;
    void init(const HdrHistConfig& config = {}) noexcept;
    void record(uint64_t value) noexcept;
    void record_with_count(uint64_t value, uint64_t count) noexcept;
    [[nodiscard]] uint64_t percentile(double pct) const noexcept;

    struct Percentiles {
        uint64_t p50, p90, p99, p999, min, max, total_count;
        double mean;
    };
    [[nodiscard]] Percentiles get_percentiles() const noexcept;
    void reset() noexcept;
    [[nodiscard]] uint64_t total_count() const noexcept { return total_count_; }
    void print_summary(double tsc_to_ns_factor = 1.0) const noexcept;

private:
    [[nodiscard]] std::size_t value_to_index(uint64_t value) const noexcept;
    [[nodiscard]] uint64_t index_to_value(std::size_t index) const noexcept;

    alignas(CACHE_LINE_SIZE) uint64_t counts_[MAX_BUCKETS] = {};
    uint64_t total_count_ = 0;
    uint64_t min_value_ = UINT64_MAX;
    uint64_t max_value_ = 0;
    uint64_t total_sum_ = 0;      // Integer sum — cast to double only in get_percentiles()
    uint64_t lowest_trackable_ = 1;
    uint64_t highest_trackable_ = 10'000'000'000ULL;
    int sub_bucket_half_count_magnitude_ = 0;
    int sub_bucket_half_count_ = 0;
    int sub_bucket_count_ = 0;
    int sub_bucket_mask_ = 0;
    int unit_magnitude_ = 0;
    std::size_t bucket_count_ = 0;
    std::size_t counts_len_ = 0;
};

} // namespace iicpc
