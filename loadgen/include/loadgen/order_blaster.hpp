#pragma once
// =============================================================================
// order_blaster.hpp — High-Throughput Order Generator (The "Firehose")
// =============================================================================
// THIS IS THE CORE OF THE PIVOT.
//
// Old model: We build the exchange, contestants write strategies.
// NEW model: Contestants build the orderbook, WE blast orders at it.
//
// This generates realistic exchange order flow:
//   - Limit orders (BUY/SELL at various price levels)
//   - Market orders (immediate execution)
//   - Cancel requests
//   - Deterministic (same seed = same order sequence = fair scoring)
//
// Uses CRTP EngineBase for zero-overhead dispatch.
// All order state in SoA arrays from HugePageArena.
// =============================================================================

#include "core/types.hpp"
#include "core/tsc.hpp"
#include "core/compiler_hints.hpp"
#include "core/spinlock.hpp"
#include "core/arena.hpp"
#include "sdk/protocol.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace iicpc {

// =============================================================================
// Order Generation Config (deterministic)
// =============================================================================
struct OrderBlasterConfig {
    // Price parameters
    int64_t  initial_mid     = 1000000;  // $100.00 scaled
    int64_t  tick_size       = 100;      // $0.01
    int64_t  price_range     = 5000;     // ±$0.50 around mid
    double   volatility      = 0.0001;   // Per-order price jitter

    // Order mix (percentages, must sum to 100)
    uint32_t limit_pct       = 60;       // Limit orders
    uint32_t market_pct      = 20;       // Market orders
    uint32_t cancel_pct      = 20;       // Cancel requests

    // Volume parameters
    int32_t  min_qty         = 1;
    int32_t  max_qty         = 100;

    // Determinism
    uint64_t seed            = 12345;

    // Rate
    uint32_t orders_per_batch = 64;      // Orders generated per batch call
};

// =============================================================================
// Order Blaster State (SoA — cache-friendly iteration)
// =============================================================================
struct BlasterState {
    // SoA arrays for active order tracking
    uint32_t* order_ids        = nullptr; // Client order IDs we've sent
    int64_t*  order_prices     = nullptr; // Prices of active orders
    Side*     order_sides      = nullptr; // BUY/SELL
    int32_t*  order_qtys       = nullptr; // Quantities
    bool*     order_active     = nullptr; // Is this slot active?

    uint32_t  active_count     = 0;
    uint32_t  capacity         = 0;
    uint32_t  next_order_id    = 1;

    bool init(HugePageArena& arena, uint32_t cap) noexcept {
        capacity = cap;
        order_ids    = arena.allocate<uint32_t>(cap);
        order_prices = arena.allocate<int64_t>(cap);
        order_sides  = arena.allocate<Side>(cap);
        order_qtys   = arena.allocate<int32_t>(cap);
        order_active = arena.allocate<bool>(cap);

        if (!order_ids || !order_prices || !order_sides ||
            !order_qtys || !order_active) return false;

        std::memset(order_active, 0, sizeof(bool) * cap);
        return true;
    }

    uint32_t find_free_slot() noexcept {
        for (uint32_t i = 0; i < capacity; ++i) {
            if (!order_active[i]) return i;
        }
        return UINT32_MAX;
    }

    uint32_t find_active_slot(uint64_t rng) noexcept {
        if (active_count == 0) return UINT32_MAX;
        // Random walk from a starting point
        uint32_t start = static_cast<uint32_t>(rng % capacity);
        for (uint32_t i = 0; i < capacity; ++i) {
            uint32_t idx = (start + i) % capacity;
            if (order_active[idx]) return idx;
        }
        return UINT32_MAX;
    }
};

// =============================================================================
// Outbound order buffer (ring of orders to send to contestant)
// =============================================================================
inline constexpr uint32_t BLAST_RING_SIZE = 65536;

struct BlastOrder {
    alignas(64) uint8_t data[64]; // OrderEntry or CancelRequest
    uint32_t size;                // Actual message size
    MsgType  type;                // ORDER_ENTRY or CANCEL_REQUEST
};

// =============================================================================
// Order Blaster — Generates order flow, writes into ring buffer
// =============================================================================
class OrderBlaster {
public:
    OrderBlaster() noexcept = default;

    bool init(HugePageArena& arena, const OrderBlasterConfig& cfg) noexcept {
        cfg_ = cfg;
        rng_ = cfg.seed;

        if (!state_.init(arena, 4096)) return false;

        ring_ = static_cast<BlastOrder*>(
            arena.allocate_raw(sizeof(BlastOrder) * BLAST_RING_SIZE,
                               CACHE_LINE_SIZE));
        if (!ring_) return false;

        write_pos_ = 0;
        read_pos_ = 0;
        mid_price_ = static_cast<double>(cfg.initial_mid);
        return true;
    }

    /// Generate a batch of orders. Call this in the hot loop.
    IICPC_HOT
    uint32_t generate_batch() noexcept {
        uint32_t generated = 0;

        for (uint32_t i = 0; i < cfg_.orders_per_batch; ++i) {
            uint32_t available = BLAST_RING_SIZE -
                (write_pos_ - read_pos_);
            if (available == 0) break; // Ring full

            // Walk the price
            double noise = next_gaussian() * cfg_.volatility *
                static_cast<double>(cfg_.initial_mid);
            mid_price_ += noise;
            if (mid_price_ < cfg_.tick_size * 10)
                mid_price_ = cfg_.tick_size * 10;

            // Decide order type
            uint32_t roll = next_raw() % 100;

            if (roll < cfg_.limit_pct) {
                generate_limit_order();
                generated++;
            } else if (roll < cfg_.limit_pct + cfg_.market_pct) {
                generate_market_order();
                generated++;
            } else {
                if (state_.active_count > 0) {
                    generate_cancel();
                    generated++;
                }
            }

            total_generated_++;
        }
        return generated;
    }

    /// Pop next order from the ring (consumer side)
    [[nodiscard]] IICPC_FORCE_INLINE
    bool pop(BlastOrder& out) noexcept {
        if (read_pos_ >= write_pos_) return false;
        out = ring_[read_pos_ % BLAST_RING_SIZE];
        read_pos_++;
        return true;
    }

    /// How many orders are queued
    [[nodiscard]] uint32_t queued() const noexcept {
        return static_cast<uint32_t>(write_pos_ - read_pos_);
    }

    [[nodiscard]] uint64_t total_generated() const noexcept {
        return total_generated_;
    }
    [[nodiscard]] uint64_t total_limits() const noexcept {
        return total_limits_;
    }
    [[nodiscard]] uint64_t total_markets() const noexcept {
        return total_markets_;
    }
    [[nodiscard]] uint64_t total_cancels() const noexcept {
        return total_cancels_;
    }

private:
    void generate_limit_order() noexcept {
        uint32_t slot = state_.find_free_slot();
        if (slot == UINT32_MAX) return;

        Side side = (next_raw() & 1) ? Side::BUY : Side::SELL;
        int64_t mid = static_cast<int64_t>(mid_price_);
        mid = (mid / cfg_.tick_size) * cfg_.tick_size;

        // Price offset: ±spread levels from mid
        int64_t offset = static_cast<int64_t>(
            (next_raw() % 20) - 10) * cfg_.tick_size;
        int64_t price = mid + offset;
        if (side == Side::BUY) price -= cfg_.tick_size;
        else price += cfg_.tick_size;

        if (price <= 0) price = cfg_.tick_size;

        int32_t qty = cfg_.min_qty + static_cast<int32_t>(
            next_raw() % static_cast<uint64_t>(cfg_.max_qty - cfg_.min_qty + 1));

        OrderEntry order{};
        order.msg_type = MsgType::ORDER_ENTRY;
        order.side = side;
        order.order_type = OrderType::LIMIT;
        order.instrument_id = 0;
        order.client_order_id = state_.next_order_id++;
        order.price = price;
        order.quantity = qty;

        // Track in state
        state_.order_ids[slot] = order.client_order_id;
        state_.order_prices[slot] = price;
        state_.order_sides[slot] = side;
        state_.order_qtys[slot] = qty;
        state_.order_active[slot] = true;
        state_.active_count++;

        // Push to ring
        auto& entry = ring_[write_pos_ % BLAST_RING_SIZE];
        std::memcpy(entry.data, &order, sizeof(order));
        entry.size = sizeof(order);
        entry.type = MsgType::ORDER_ENTRY;
        write_pos_++;
        total_limits_++;
    }

    void generate_market_order() noexcept {
        Side side = (next_raw() & 1) ? Side::BUY : Side::SELL;
        int32_t qty = cfg_.min_qty + static_cast<int32_t>(
            next_raw() % static_cast<uint64_t>(cfg_.max_qty - cfg_.min_qty + 1));

        OrderEntry order{};
        order.msg_type = MsgType::ORDER_ENTRY;
        order.side = side;
        order.order_type = OrderType::MARKET;
        order.instrument_id = 0;
        order.client_order_id = state_.next_order_id++;
        order.price = 0; // Market order — no price
        order.quantity = qty;

        auto& entry = ring_[write_pos_ % BLAST_RING_SIZE];
        std::memcpy(entry.data, &order, sizeof(order));
        entry.size = sizeof(order);
        entry.type = MsgType::ORDER_ENTRY;
        write_pos_++;
        total_markets_++;
    }

    void generate_cancel() noexcept {
        uint32_t slot = state_.find_active_slot(next_raw());
        if (slot == UINT32_MAX) return;

        CancelRequest cancel{};
        cancel.msg_type = MsgType::CANCEL_REQUEST;
        cancel.client_order_id = state_.order_ids[slot];
        cancel.exchange_order_id = 0; // Contestant assigns this

        // Remove from tracking
        state_.order_active[slot] = false;
        state_.active_count--;

        auto& entry = ring_[write_pos_ % BLAST_RING_SIZE];
        std::memcpy(entry.data, &cancel, sizeof(cancel));
        entry.size = sizeof(cancel);
        entry.type = MsgType::CANCEL_REQUEST;
        write_pos_++;
        total_cancels_++;
    }

    // xorshift64 PRNG — fast, deterministic
    uint64_t next_raw() noexcept {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 7;
        rng_ ^= rng_ << 17;
        return rng_;
    }

    double next_uniform() noexcept {
        return static_cast<double>(next_raw() & 0xFFFFFFFF) / 4294967296.0;
    }

    double next_gaussian() noexcept {
        double u1 = next_uniform();
        double u2 = next_uniform();
        if (u1 < 1e-10) u1 = 1e-10;
        return std::sqrt(-2.0 * std::log(u1)) *
               std::cos(2.0 * 3.14159265358979 * u2);
    }

    OrderBlasterConfig cfg_{};
    BlasterState state_;
    BlastOrder* ring_ = nullptr;
    uint64_t write_pos_ = 0;
    uint64_t read_pos_ = 0;
    double mid_price_ = 0.0;
    uint64_t rng_ = 0;
    uint64_t total_generated_ = 0;
    uint64_t total_limits_ = 0;
    uint64_t total_markets_ = 0;
    uint64_t total_cancels_ = 0;
};

} // namespace iicpc
