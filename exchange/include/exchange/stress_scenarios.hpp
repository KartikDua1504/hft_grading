#pragma once
// stress_scenarios.hpp — Adversarial Stress Test Scenario Library
// CF-style "system tests" — 10 deterministic scenarios that probe edge cases
// in contestant orderbook implementations. Each scenario uses a fixed seed
// and specific OrderBlasterConfig overrides to generate reproducible,
// adversarial order streams.
//
// Scenarios:
//   1. Crossed Book Stress   — Rapid overlapping buy/sell at same prices
//   2. Deep Book Sweep       — Market orders that sweep 1000+ levels
//   3. Cancel Storm          — 90% cancel rate, tests cancel-rebooking
//   4. Self-Trade Trap       — Orders near position limits, tight spread
//   5. Tick-Size Edge        — Min/max price boundaries, tick alignment
//   6. Burst Traffic         — Short-duration, extreme OPS spike
//   7. IOC Flood             — Pure IOC/Market orders, nothing should rest
//   8. Duplicate Order IDs   — Rapidly reusing client_order_id space
//   9. Position Limit Grind  — Orders exactly at position limits
//  10. Conservation Audit    — Balanced buy/sell, verify qty conservation

#include "loadgen/order_blaster.hpp"

#include <cstdint>
#include <cstring>

namespace iicpc {

// Scenario Definition
inline constexpr uint32_t MAX_STRESS_SCENARIOS = 10;

struct StressScenario {
    uint32_t           id;
    char               name[48];
    char               description[128];
    double             weight;          // Contribution to system test score
    uint32_t           duration_secs;   // How long to run this scenario
    OrderBlasterConfig blaster_cfg;     // Overridden blaster config
};

// Per-scenario result
struct ScenarioResult {
    uint32_t scenario_id      = 0;
    char     name[48]         = {};
    bool     passed           = false;
    double   correctness      = 0.0;    // ShadowOrderbook correctness_score()
    double   weighted_score   = 0.0;    // correctness * weight
    uint64_t orders_sent      = 0;
    uint64_t responses_recv   = 0;
    uint64_t correct_fills    = 0;
    uint64_t wrong_fills      = 0;
    uint64_t missing_fills    = 0;
    uint64_t extra_fills      = 0;
    uint64_t priority_errors  = 0;
};

// Build the 10 stress scenarios
inline uint32_t build_stress_scenarios(StressScenario* out) noexcept {
    uint32_t count = 0;

    // Scenario 1: Crossed Book Stress
    // Orders alternate BUY/SELL at overlapping prices. Tests that the
    // contestant correctly matches crosses instead of letting them rest.
    {
        auto& s = out[count++];
        s.id = 1;
        std::strncpy(s.name, "Crossed Book Stress", sizeof(s.name) - 1);
        std::strncpy(s.description,
            "Rapid overlapping buy/sell at same prices — tests crossing logic",
            sizeof(s.description) - 1);
        s.weight = 0.15;
        s.duration_secs = 8;
        s.blaster_cfg.seed = 0xDEADBEEF'CAFE0001ULL;
        s.blaster_cfg.initial_mid = 1000000;
        s.blaster_cfg.tick_size = 100;
        s.blaster_cfg.price_range = 200;       // Very tight range → frequent crosses
        s.blaster_cfg.volatility = 0.00001;    // Minimal drift
        s.blaster_cfg.limit_pct = 90;
        s.blaster_cfg.market_pct = 10;
        s.blaster_cfg.cancel_pct = 0;
        s.blaster_cfg.min_qty = 1;
        s.blaster_cfg.max_qty = 10;
        s.blaster_cfg.orders_per_batch = 256;
    }

    // Scenario 2: Deep Book Sweep
    // Builds a deep book with many price levels, then sends large market
    // orders that sweep through hundreds of levels.
    {
        auto& s = out[count++];
        s.id = 2;
        std::strncpy(s.name, "Deep Book Sweep", sizeof(s.name) - 1);
        std::strncpy(s.description,
            "Large market orders sweeping 100+ price levels",
            sizeof(s.description) - 1);
        s.weight = 0.12;
        s.duration_secs = 10;
        s.blaster_cfg.seed = 0xDEADBEEF'CAFE0002ULL;
        s.blaster_cfg.initial_mid = 1000000;
        s.blaster_cfg.tick_size = 100;
        s.blaster_cfg.price_range = 50000;     // Wide range → many levels
        s.blaster_cfg.volatility = 0.0;        // No drift — static book
        s.blaster_cfg.limit_pct = 40;
        s.blaster_cfg.market_pct = 60;         // Heavy market order load
        s.blaster_cfg.cancel_pct = 0;
        s.blaster_cfg.min_qty = 50;
        s.blaster_cfg.max_qty = 500;           // Large sweeping orders
        s.blaster_cfg.orders_per_batch = 128;
    }

    // Scenario 3: Cancel Storm
    // 90% cancel rate. Tests that cancel handling doesn't corrupt the book
    // state, and that cancelled orders are properly removed.
    {
        auto& s = out[count++];
        s.id = 3;
        std::strncpy(s.name, "Cancel Storm", sizeof(s.name) - 1);
        std::strncpy(s.description,
            "90% cancel rate — tests cancel-rebooking integrity",
            sizeof(s.description) - 1);
        s.weight = 0.10;
        s.duration_secs = 8;
        s.blaster_cfg.seed = 0xDEADBEEF'CAFE0003ULL;
        s.blaster_cfg.initial_mid = 1000000;
        s.blaster_cfg.tick_size = 100;
        s.blaster_cfg.price_range = 5000;
        s.blaster_cfg.volatility = 0.0001;
        s.blaster_cfg.limit_pct = 8;
        s.blaster_cfg.market_pct = 2;
        s.blaster_cfg.cancel_pct = 90;         // Extreme cancel rate
        s.blaster_cfg.min_qty = 1;
        s.blaster_cfg.max_qty = 50;
        s.blaster_cfg.orders_per_batch = 256;
    }

    // Scenario 4: Self-Trade Trap
    // Tight spread, high volume near position limits. Forces contestants to
    // handle near-limit positions where orders nearly self-trade.
    {
        auto& s = out[count++];
        s.id = 4;
        std::strncpy(s.name, "Self-Trade Trap", sizeof(s.name) - 1);
        std::strncpy(s.description,
            "Tight spread, high volume near position limits",
            sizeof(s.description) - 1);
        s.weight = 0.08;
        s.duration_secs = 8;
        s.blaster_cfg.seed = 0xDEADBEEF'CAFE0004ULL;
        s.blaster_cfg.initial_mid = 500000;
        s.blaster_cfg.tick_size = 50;
        s.blaster_cfg.price_range = 100;        // Extremely tight spread
        s.blaster_cfg.volatility = 0.0;
        s.blaster_cfg.limit_pct = 80;
        s.blaster_cfg.market_pct = 15;
        s.blaster_cfg.cancel_pct = 5;
        s.blaster_cfg.min_qty = 80;
        s.blaster_cfg.max_qty = 100;            // Large qty near limits
        s.blaster_cfg.orders_per_batch = 64;
    }

    // Scenario 5: Tick-Size Edge
    // Orders at min price (1 tick), max price (near INT64_MAX/2),
    // and exactly on tick boundaries. Tests boundary handling.
    {
        auto& s = out[count++];
        s.id = 5;
        std::strncpy(s.name, "Tick-Size Edge", sizeof(s.name) - 1);
        std::strncpy(s.description,
            "Orders at min/max price boundaries, tick alignment stress",
            sizeof(s.description) - 1);
        s.weight = 0.08;
        s.duration_secs = 6;
        s.blaster_cfg.seed = 0xDEADBEEF'CAFE0005ULL;
        s.blaster_cfg.initial_mid = 100;        // Very low starting price
        s.blaster_cfg.tick_size = 1;            // Minimum tick
        s.blaster_cfg.price_range = 50;
        s.blaster_cfg.volatility = 0.001;       // High relative vol
        s.blaster_cfg.limit_pct = 70;
        s.blaster_cfg.market_pct = 20;
        s.blaster_cfg.cancel_pct = 10;
        s.blaster_cfg.min_qty = 1;
        s.blaster_cfg.max_qty = 5;
        s.blaster_cfg.orders_per_batch = 128;
    }

    // Scenario 6: Burst Traffic
    // Short duration but extreme batch size. Tests throughput ceiling
    // and backpressure handling under spike conditions.
    {
        auto& s = out[count++];
        s.id = 6;
        std::strncpy(s.name, "Burst Traffic", sizeof(s.name) - 1);
        std::strncpy(s.description,
            "Short-duration extreme OPS spike — tests throughput ceiling",
            sizeof(s.description) - 1);
        s.weight = 0.12;
        s.duration_secs = 5;
        s.blaster_cfg.seed = 0xDEADBEEF'CAFE0006ULL;
        s.blaster_cfg.initial_mid = 1000000;
        s.blaster_cfg.tick_size = 100;
        s.blaster_cfg.price_range = 5000;
        s.blaster_cfg.volatility = 0.0001;
        s.blaster_cfg.limit_pct = 60;
        s.blaster_cfg.market_pct = 20;
        s.blaster_cfg.cancel_pct = 20;
        s.blaster_cfg.min_qty = 1;
        s.blaster_cfg.max_qty = 50;
        s.blaster_cfg.orders_per_batch = 512;   // Very high batch size
    }

    // Scenario 7: IOC Flood
    // 100% IOC/Market orders — nothing should rest in the book.
    // Tests that the book stays empty and all unfilled IOCs are properly
    // rejected or cancelled.
    {
        auto& s = out[count++];
        s.id = 7;
        std::strncpy(s.name, "IOC Flood", sizeof(s.name) - 1);
        std::strncpy(s.description,
            "Pure IOC/Market orders — nothing should rest in book",
            sizeof(s.description) - 1);
        s.weight = 0.08;
        s.duration_secs = 6;
        s.blaster_cfg.seed = 0xDEADBEEF'CAFE0007ULL;
        s.blaster_cfg.initial_mid = 1000000;
        s.blaster_cfg.tick_size = 100;
        s.blaster_cfg.price_range = 2000;
        s.blaster_cfg.volatility = 0.0001;
        s.blaster_cfg.limit_pct = 0;
        s.blaster_cfg.market_pct = 100;         // All market orders
        s.blaster_cfg.cancel_pct = 0;
        s.blaster_cfg.min_qty = 1;
        s.blaster_cfg.max_qty = 20;
        s.blaster_cfg.orders_per_batch = 128;
    }

    // Scenario 8: Rapid Order ID Churn
    // High volume with small order_id space. Tests that contestant
    // handles order_id recycling and doesn't confuse old/new IDs.
    {
        auto& s = out[count++];
        s.id = 8;
        std::strncpy(s.name, "Rapid Order ID Churn", sizeof(s.name) - 1);
        std::strncpy(s.description,
            "High volume, small ID space — tests order_id recycling",
            sizeof(s.description) - 1);
        s.weight = 0.08;
        s.duration_secs = 8;
        s.blaster_cfg.seed = 0xDEADBEEF'CAFE0008ULL;
        s.blaster_cfg.initial_mid = 1000000;
        s.blaster_cfg.tick_size = 100;
        s.blaster_cfg.price_range = 3000;
        s.blaster_cfg.volatility = 0.0001;
        s.blaster_cfg.limit_pct = 40;
        s.blaster_cfg.market_pct = 20;
        s.blaster_cfg.cancel_pct = 40;          // Frequent cancel+reuse
        s.blaster_cfg.min_qty = 1;
        s.blaster_cfg.max_qty = 30;
        s.blaster_cfg.orders_per_batch = 256;
    }

    // Scenario 9: Position Limit Grind
    // Alternating large BUY then large SELL to push position to the edge.
    // Tests that fills near the limit are correctly handled.
    {
        auto& s = out[count++];
        s.id = 9;
        std::strncpy(s.name, "Position Limit Grind", sizeof(s.name) - 1);
        std::strncpy(s.description,
            "Orders exactly at position limit boundaries",
            sizeof(s.description) - 1);
        s.weight = 0.09;
        s.duration_secs = 8;
        s.blaster_cfg.seed = 0xDEADBEEF'CAFE0009ULL;
        s.blaster_cfg.initial_mid = 1000000;
        s.blaster_cfg.tick_size = 100;
        s.blaster_cfg.price_range = 1000;
        s.blaster_cfg.volatility = 0.0;
        s.blaster_cfg.limit_pct = 70;
        s.blaster_cfg.market_pct = 30;
        s.blaster_cfg.cancel_pct = 0;
        s.blaster_cfg.min_qty = 90;
        s.blaster_cfg.max_qty = 100;            // Near max position per fill
        s.blaster_cfg.orders_per_batch = 64;
    }

    // Scenario 10: Conservation Audit
    // Balanced 50/50 buy/sell, moderate volume. After all fills,
    // total buy qty MUST equal total sell qty (conservation invariant).
    {
        auto& s = out[count++];
        s.id = 10;
        std::strncpy(s.name, "Conservation Audit", sizeof(s.name) - 1);
        std::strncpy(s.description,
            "Balanced buy/sell — total filled buy qty must equal sell qty",
            sizeof(s.description) - 1);
        s.weight = 0.10;
        s.duration_secs = 10;
        s.blaster_cfg.seed = 0xDEADBEEF'CAFE000AULL;
        s.blaster_cfg.initial_mid = 1000000;
        s.blaster_cfg.tick_size = 100;
        s.blaster_cfg.price_range = 5000;
        s.blaster_cfg.volatility = 0.00005;
        s.blaster_cfg.limit_pct = 50;
        s.blaster_cfg.market_pct = 40;
        s.blaster_cfg.cancel_pct = 10;
        s.blaster_cfg.min_qty = 10;
        s.blaster_cfg.max_qty = 100;
        s.blaster_cfg.orders_per_batch = 128;
    }

    return count;
}

} // namespace iicpc
