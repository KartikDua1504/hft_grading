#pragma once

// --- Deterministic Synthetic Market Data Generator ---
// Generates an Ornstein-Uhlenbeck price walk.
// Deterministic: same seed produces identical sequence for fair scoring.
// Zero heap allocation.

#include "sdk/protocol.hpp"
#include "core/compiler_hints.hpp"

#include <cstdint>
#include <cmath>

namespace iicpc {

struct MarketDataConfig {
    int64_t  initial_price    = 1000000;  // $100.00 in PRICE_MULTIPLIER
    int64_t  tick_size        = 100;      // $0.01
    int64_t  initial_spread   = 200;      // $0.02
    double   volatility       = 0.0002;   // Per-tick volatility
    double   mean_reversion   = 0.01;     // Mean-reversion strength
    int32_t  base_qty         = 100;      // Base quantity at each level
    uint64_t seed             = 42;       // RNG seed for determinism
};

class MarketDataGenerator {
public:
    MarketDataGenerator() noexcept = default;

    void init(const MarketDataConfig& cfg) noexcept {
        cfg_ = cfg;
        mid_price_ = static_cast<double>(cfg.initial_price);
        rng_state_ = cfg.seed;
        sequence_ = 0;
    }

    /// Generate next market data tick. Deterministic given seed.
    IICPC_HOT
    MarketUpdate next_tick(uint64_t timestamp_ns) noexcept {
        // Ornstein-Uhlenbeck price process
        double noise = next_gaussian() * cfg_.volatility *
                       static_cast<double>(cfg_.initial_price);
        double reversion = cfg_.mean_reversion *
                          (static_cast<double>(cfg_.initial_price) - mid_price_);
        mid_price_ += noise + reversion;

        // Ensure price stays positive
        if (mid_price_ < static_cast<double>(cfg_.tick_size * 10)) {
            mid_price_ = static_cast<double>(cfg_.tick_size * 10);
        }

        // Snap to tick size
        int64_t mid = static_cast<int64_t>(mid_price_);
        mid = (mid / cfg_.tick_size) * cfg_.tick_size;

        // Variable spread
        int64_t half_spread = cfg_.initial_spread / 2;
        half_spread += static_cast<int64_t>(next_uniform() * 3.0) * cfg_.tick_size;

        MarketUpdate update{};
        update.msg_type = MsgType::MARKET_UPDATE;
        update.instrument_id = 0;
        update.sequence = sequence_++;
        update.best_bid_price = mid - half_spread;
        update.best_ask_price = mid + half_spread;
        update.best_bid_qty = cfg_.base_qty +
            static_cast<int32_t>(next_uniform() * 50.0);
        update.best_ask_qty = cfg_.base_qty +
            static_cast<int32_t>(next_uniform() * 50.0);
        update.last_trade_price = mid;
        update.last_trade_qty = static_cast<int32_t>(next_uniform() * 20.0) + 1;
        update.exchange_ts_ns = timestamp_ns;

        last_update_ = update;
        return update;
    }

    [[nodiscard]] const MarketUpdate& last_update() const noexcept {
        return last_update_;
    }

    [[nodiscard]] uint32_t sequence() const noexcept { return sequence_; }

private:
    // xorshift PRNG — fast, deterministic
    uint64_t next_raw() noexcept {
        uint64_t s = rng_state_;
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        rng_state_ = s;
        return s;
    }

    double next_uniform() noexcept {
        return static_cast<double>(next_raw() & 0xFFFFFFFF) / 4294967296.0;
    }

    // Box-Muller transform for Gaussian noise
    double next_gaussian() noexcept {
        double u1 = next_uniform();
        double u2 = next_uniform();
        if (u1 < 1e-10) u1 = 1e-10;
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * 3.14159265358979 * u2);
    }

    MarketDataConfig cfg_{};
    double mid_price_ = 0.0;
    uint64_t rng_state_ = 0;
    uint32_t sequence_ = 0;
    MarketUpdate last_update_{};
};

} // namespace iicpc
