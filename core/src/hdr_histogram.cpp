// =============================================================================
// hdr_histogram.cpp — HDR Histogram Implementation
// =============================================================================
#include "core/hdr_histogram.hpp"
#include <cmath>
#include <algorithm>

namespace iicpc {

void HdrHistogram::init(const HdrHistConfig& config) noexcept {
    lowest_trackable_ = config.lowest_trackable;
    highest_trackable_ = config.highest_trackable;

    // Calculate sub-bucket structure based on significant figures
    const int64_t largest_value_with_single_unit_resolution =
        2 * static_cast<int64_t>(std::pow(10.0, config.significant_figures));

    int sub_bucket_count_magnitude = static_cast<int>(
        std::ceil(std::log2(static_cast<double>(largest_value_with_single_unit_resolution))));
    
    sub_bucket_half_count_magnitude_ = (sub_bucket_count_magnitude > 1)
        ? (sub_bucket_count_magnitude - 1) : 0;
    sub_bucket_count_ = 1 << (sub_bucket_half_count_magnitude_ + 1);
    sub_bucket_half_count_ = sub_bucket_count_ / 2;
    sub_bucket_mask_ = (sub_bucket_count_ - 1) << unit_magnitude_;

    unit_magnitude_ = static_cast<int>(
        std::max(0.0, std::floor(std::log2(static_cast<double>(lowest_trackable_)))));

    // Calculate number of bucket levels
    uint64_t smallest_untrackable = static_cast<uint64_t>(sub_bucket_count_)
                                    << unit_magnitude_;
    bucket_count_ = 1;
    while (smallest_untrackable <= highest_trackable_) {
        if (smallest_untrackable > UINT64_MAX / 2) break;
        smallest_untrackable <<= 1;
        bucket_count_++;
    }

    counts_len_ = (bucket_count_ + 1) * sub_bucket_half_count_;
    if (counts_len_ > MAX_BUCKETS) counts_len_ = MAX_BUCKETS;

    reset();
}

void HdrHistogram::record(uint64_t value) noexcept {
    record_with_count(value, 1);
}

void HdrHistogram::record_with_count(uint64_t value, uint64_t count) noexcept {
    const std::size_t idx = value_to_index(value);
    if (idx < counts_len_) {
        counts_[idx] += count;
        total_count_ += count;
        total_sum_ += value * count;
        if (value < min_value_) min_value_ = value;
        if (value > max_value_) max_value_ = value;
    }
}

uint64_t HdrHistogram::percentile(double pct) const noexcept {
    if (total_count_ == 0) return 0;

    const uint64_t target = std::max(
        static_cast<uint64_t>(1),
        static_cast<uint64_t>(std::ceil(pct / 100.0 * static_cast<double>(total_count_))));

    uint64_t cumulative = 0;
    for (std::size_t i = 0; i < counts_len_; ++i) {
        cumulative += counts_[i];
        if (cumulative >= target) {
            return index_to_value(i);
        }
    }
    return max_value_;
}

HdrHistogram::Percentiles HdrHistogram::get_percentiles() const noexcept {
    Percentiles p{};
    if (total_count_ == 0) return p;

    p.min = min_value_;
    p.max = max_value_;
    p.total_count = total_count_;
    p.mean = static_cast<double>(total_sum_) / static_cast<double>(total_count_);

    const uint64_t t50  = static_cast<uint64_t>(std::ceil(0.50 * static_cast<double>(total_count_)));
    const uint64_t t90  = static_cast<uint64_t>(std::ceil(0.90 * static_cast<double>(total_count_)));
    const uint64_t t99  = static_cast<uint64_t>(std::ceil(0.99 * static_cast<double>(total_count_)));
    const uint64_t t999 = static_cast<uint64_t>(std::ceil(0.999 * static_cast<double>(total_count_)));

    uint64_t cumulative = 0;
    bool got50 = false, got90 = false, got99 = false, got999 = false;

    for (std::size_t i = 0; i < counts_len_; ++i) {
        cumulative += counts_[i];
        const uint64_t val = index_to_value(i);
        if (!got50  && cumulative >= t50)  { p.p50  = val; got50  = true; }
        if (!got90  && cumulative >= t90)  { p.p90  = val; got90  = true; }
        if (!got99  && cumulative >= t99)  { p.p99  = val; got99  = true; }
        if (!got999 && cumulative >= t999) { p.p999 = val; got999 = true; }
        if (got999) break;
    }
    return p;
}

void HdrHistogram::reset() noexcept {
    std::memset(counts_, 0, sizeof(uint64_t) * counts_len_);
    total_count_ = 0;
    min_value_ = UINT64_MAX;
    max_value_ = 0;
    total_sum_ = 0;
}

std::size_t HdrHistogram::value_to_index(uint64_t value) const noexcept {
    if (value < static_cast<uint64_t>(sub_bucket_count_) << unit_magnitude_) {
        // First bucket: linear indexing
        return static_cast<std::size_t>(value >> unit_magnitude_);
    }

    // Find the bucket level via leading zeros
    const int leading = __builtin_clzll(static_cast<unsigned long long>(value));
    const int bucket_idx = 63 - leading - sub_bucket_half_count_magnitude_ - unit_magnitude_;
    const int sub_idx = static_cast<int>(value >> (bucket_idx + unit_magnitude_))
                        - sub_bucket_half_count_;
    
    std::size_t idx = static_cast<std::size_t>(
        sub_bucket_half_count_ * (bucket_idx + 1) + sub_idx);
    return std::min(idx, counts_len_ - 1);
}

uint64_t HdrHistogram::index_to_value(std::size_t index) const noexcept {
    if (index < static_cast<std::size_t>(sub_bucket_count_)) {
        return static_cast<uint64_t>(index) << unit_magnitude_;
    }

    const int bucket_idx = static_cast<int>(index / static_cast<std::size_t>(sub_bucket_half_count_)) - 1;
    const int sub_idx = static_cast<int>(index % static_cast<std::size_t>(sub_bucket_half_count_))
                        + sub_bucket_half_count_;
    
    return static_cast<uint64_t>(sub_idx) << (bucket_idx + unit_magnitude_);
}

void HdrHistogram::print_summary(double tsc_to_ns_factor) const noexcept {
    if (total_count_ == 0) {
        std::fprintf(stderr, "[hdr] No samples recorded\n");
        return;
    }
    auto p = get_percentiles();
    auto to_us = [&](uint64_t v) { return static_cast<double>(v) * tsc_to_ns_factor / 1000.0; };
    std::fprintf(stderr,
        "[hdr] samples=%lu  min=%.1fus  p50=%.1fus  p90=%.1fus  p99=%.1fus  p999=%.1fus  max=%.1fus\n",
        p.total_count,
        to_us(p.min), to_us(p.p50), to_us(p.p90),
        to_us(p.p99), to_us(p.p999), to_us(p.max));
}

} // namespace iicpc
