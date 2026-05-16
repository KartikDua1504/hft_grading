#pragma once
// =============================================================================
// tsc.hpp — Hardware Cycle Counter (RDTSC) Wrappers
// =============================================================================
// We measure latency by reading the CPU's Time Stamp Counter directly.
// This gives cycle-accurate measurements without kernel overhead.
//
// On Alder Lake with constant_tsc and nonstop_tsc flags (verified on this CPU),
// the TSC ticks at a constant rate regardless of frequency scaling.
//
// We calibrate TSC-to-nanoseconds once at startup using clock_gettime as
// a reference, then never touch the kernel clock again on the hot path.
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <x86intrin.h>
#include <time.h>

namespace iicpc {

/// Read the Time Stamp Counter. This is a single instruction, ~1 cycle latency.
/// NOT serializing — use rdtscp() if you need a fence.
inline uint64_t rdtsc() noexcept {
    return __rdtsc();
}

/// Read TSC with serialization (RDTSCP). Ensures all prior instructions
/// have completed before reading the counter. Slightly more expensive (~20 cycles)
/// but prevents out-of-order measurement artifacts.
inline uint64_t rdtscp() noexcept {
    unsigned aux;
    return __rdtscp(&aux);
}

/// TSC calibration result. Computed once at startup.
struct TscCalibration {
    double tsc_to_ns;      // Multiply TSC ticks by this to get nanoseconds
    double tsc_ghz;        // TSC frequency in GHz
    uint64_t tsc_hz;       // TSC frequency in Hz (integer)
};

/// Calibrate the TSC frequency by measuring against CLOCK_MONOTONIC.
/// This takes ~100ms and should only be called once at startup.
inline TscCalibration calibrate_tsc() noexcept {
    constexpr int64_t CALIBRATION_NS = 100'000'000LL; // 100ms

    struct timespec ts_start, ts_end;
    
    // Warm up the TSC pipeline
    __rdtsc();
    __rdtsc();
    __rdtsc();

    // Take reference measurement
    ::clock_gettime(CLOCK_MONOTONIC, &ts_start);
    const uint64_t tsc_start = rdtscp();

    // Busy-wait for calibration period
    struct timespec ts_now;
    do {
        ::clock_gettime(CLOCK_MONOTONIC, &ts_now);
    } while ((ts_now.tv_sec - ts_start.tv_sec) * 1'000'000'000LL +
             (ts_now.tv_nsec - ts_start.tv_nsec) < CALIBRATION_NS);

    const uint64_t tsc_end = rdtscp();
    ::clock_gettime(CLOCK_MONOTONIC, &ts_end);

    // Calculate TSC frequency
    const int64_t elapsed_ns = (ts_end.tv_sec - ts_start.tv_sec) * 1'000'000'000LL +
                               (ts_end.tv_nsec - ts_start.tv_nsec);
    const uint64_t elapsed_tsc = tsc_end - tsc_start;

    const double tsc_hz = static_cast<double>(elapsed_tsc) * 1e9 / static_cast<double>(elapsed_ns);
    const double tsc_to_ns = static_cast<double>(elapsed_ns) / static_cast<double>(elapsed_tsc);
    const double tsc_ghz = tsc_hz / 1e9;

    std::fprintf(stderr, "[tsc] Calibrated: %.3f GHz (%.3f ns/tick, measured over %ld ms)\n",
                 tsc_ghz, tsc_to_ns, elapsed_ns / 1'000'000);

    return TscCalibration{
        .tsc_to_ns = tsc_to_ns,
        .tsc_ghz = tsc_ghz,
        .tsc_hz = static_cast<uint64_t>(tsc_hz),
    };
}

/// Convert TSC tick delta to nanoseconds using pre-calibrated value
inline double tsc_to_ns(uint64_t ticks, const TscCalibration& cal) noexcept {
    return static_cast<double>(ticks) * cal.tsc_to_ns;
}

/// Convert TSC tick delta to microseconds
inline double tsc_to_us(uint64_t ticks, const TscCalibration& cal) noexcept {
    return tsc_to_ns(ticks, cal) / 1000.0;
}

} // namespace iicpc
