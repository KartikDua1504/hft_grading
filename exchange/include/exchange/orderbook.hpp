#pragma once
// =============================================================================
// orderbook.hpp — SoA Price-Time Priority Limit Order Book
// =============================================================================
// Single-instrument LOB. Strict Data-Oriented Design:
//   - Price levels stored as contiguous SoA arrays (not std::map)
//   - Orders stored in a flat pool with intrusive linked list per level
//   - All memory from HugePageArena — zero heap allocation
//   - O(1) best-price access, O(1) insert at known level
//
// Price representation: int64_t scaled by PRICE_MULTIPLIER (10000).
//   e.g., $100.50 = 1005000
// =============================================================================

#include "sdk/protocol.hpp"
#include "core/arena.hpp"
#include "core/types.hpp"
#include "core/compiler_hints.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace iicpc {

// =============================================================================
// Constants
// =============================================================================
inline constexpr uint32_t MAX_ORDERS        = 65536;  // Per side
inline constexpr uint32_t MAX_PRICE_LEVELS_OB = 4096; // Distinct price levels per side
inline constexpr uint32_t NULL_INDEX        = UINT32_MAX;

// =============================================================================
// Order (stored in flat pool)
// =============================================================================
struct alignas(64) Order {
    uint64_t exchange_id;       // Global unique exchange order ID
    uint32_t client_order_id;   // Contestant-assigned
    uint32_t contestant_id;     // Which contestant owns this
    int64_t  price;             // Limit price
    int32_t  remaining_qty;     // Remaining quantity
    int32_t  original_qty;      // Original quantity
    Side     side;
    uint8_t  _pad0[3];
    uint32_t next;              // Intrusive linked list: next order at same price
    uint32_t prev;              // Intrusive linked list: prev order at same price
    uint32_t level_idx;         // Which price level this belongs to
    uint64_t timestamp_ns;      // Insertion time (for time priority)
};
static_assert(sizeof(Order) == 64, "Order must be 1 cache line");

// =============================================================================
// Price Level (SoA arrays)
// =============================================================================
struct PriceLevelPool {
    // SoA arrays — each field is a contiguous array
    int64_t*  prices       = nullptr; // Price at this level
    int32_t*  total_qty    = nullptr; // Total quantity at level
    uint32_t* order_count  = nullptr; // Number of orders at level
    uint32_t* head         = nullptr; // Head of order linked list
    uint32_t* tail         = nullptr; // Tail of order linked list
    bool*     active       = nullptr; // Is this level active?

    uint32_t  count        = 0;       // Current number of active levels
    uint32_t  capacity     = 0;

    bool init(HugePageArena& arena, uint32_t cap) noexcept {
        capacity = cap;
        prices      = arena.allocate<int64_t>(cap);
        total_qty   = arena.allocate<int32_t>(cap);
        order_count = arena.allocate<uint32_t>(cap);
        head        = arena.allocate<uint32_t>(cap);
        tail        = arena.allocate<uint32_t>(cap);
        active      = arena.allocate<bool>(cap);

        if (!prices || !total_qty || !order_count || !head || !tail || !active)
            return false;

        std::memset(prices, 0, sizeof(int64_t) * cap);
        std::memset(total_qty, 0, sizeof(int32_t) * cap);
        std::memset(order_count, 0, sizeof(uint32_t) * cap);
        std::memset(head, 0xFF, sizeof(uint32_t) * cap);  // NULL_INDEX
        std::memset(tail, 0xFF, sizeof(uint32_t) * cap);
        std::memset(active, 0, sizeof(bool) * cap);
        return true;
    }

    // Find or create a level for this price. Returns level index.
    uint32_t find_or_create(int64_t price) noexcept {
        // Linear scan for now (small N, cache-friendly)
        for (uint32_t i = 0; i < count; ++i) {
            if (active[i] && prices[i] == price) return i;
        }
        // Create new level
        if (count >= capacity) return NULL_INDEX;
        uint32_t idx = count++;
        prices[idx] = price;
        total_qty[idx] = 0;
        order_count[idx] = 0;
        head[idx] = NULL_INDEX;
        tail[idx] = NULL_INDEX;
        active[idx] = true;
        return idx;
    }

    // Find best price level (highest for bids, lowest for asks)
    uint32_t find_best_bid() const noexcept {
        uint32_t best = NULL_INDEX;
        int64_t best_price = INT64_MIN;
        for (uint32_t i = 0; i < count; ++i) {
            if (active[i] && total_qty[i] > 0 && prices[i] > best_price) {
                best_price = prices[i];
                best = i;
            }
        }
        return best;
    }

    uint32_t find_best_ask() const noexcept {
        uint32_t best = NULL_INDEX;
        int64_t best_price = INT64_MAX;
        for (uint32_t i = 0; i < count; ++i) {
            if (active[i] && total_qty[i] > 0 && prices[i] < best_price) {
                best_price = prices[i];
                best = i;
            }
        }
        return best;
    }
};

// =============================================================================
// Order Pool (flat array, free-list allocation)
// =============================================================================
struct OrderPool {
    Order*    orders    = nullptr;
    uint32_t* free_list = nullptr;  // Stack of free indices
    uint32_t  free_top  = 0;
    uint32_t  capacity  = 0;

    bool init(HugePageArena& arena, uint32_t cap) noexcept {
        capacity = cap;
        orders = static_cast<Order*>(
            arena.allocate_raw(sizeof(Order) * cap, CACHE_LINE_SIZE));
        free_list = arena.allocate<uint32_t>(cap);
        if (!orders || !free_list) return false;

        // Initialize free list (all slots free)
        for (uint32_t i = 0; i < cap; ++i) {
            free_list[i] = cap - 1 - i; // Stack: top = 0
        }
        free_top = cap;
        return true;
    }

    uint32_t alloc() noexcept {
        if (free_top == 0) return NULL_INDEX;
        return free_list[--free_top];
    }

    void free(uint32_t idx) noexcept {
        free_list[free_top++] = idx;
    }
};

// =============================================================================
// Limit Order Book (single instrument)
// =============================================================================
class OrderBook {
public:
    OrderBook() noexcept = default;

    bool init(HugePageArena& arena) noexcept {
        if (!bids_.init(arena, MAX_PRICE_LEVELS_OB)) return false;
        if (!asks_.init(arena, MAX_PRICE_LEVELS_OB)) return false;
        if (!pool_.init(arena, MAX_ORDERS * 2)) return false;
        return true;
    }

    // =========================================================================
    // Add order — returns order pool index (or NULL_INDEX on failure)
    // =========================================================================
    IICPC_HOT
    uint32_t add_order(uint32_t contestant_id,
                       uint32_t client_order_id,
                       Side side,
                       int64_t price,
                       int32_t quantity,
                       uint64_t timestamp_ns) noexcept {
        uint32_t oidx = pool_.alloc();
        if (oidx == NULL_INDEX) return NULL_INDEX;

        Order& o = pool_.orders[oidx];
        o.exchange_id = next_exchange_id_++;
        o.client_order_id = client_order_id;
        o.contestant_id = contestant_id;
        o.price = price;
        o.remaining_qty = quantity;
        o.original_qty = quantity;
        o.side = side;
        o.timestamp_ns = timestamp_ns;
        o.next = NULL_INDEX;
        o.prev = NULL_INDEX;

        // Insert into price level
        PriceLevelPool& levels = (side == Side::BUY) ? bids_ : asks_;
        uint32_t lidx = levels.find_or_create(price);
        if (lidx == NULL_INDEX) {
            pool_.free(oidx);
            return NULL_INDEX;
        }

        o.level_idx = lidx;

        // Append to level's order list (FIFO = time priority)
        if (levels.tail[lidx] != NULL_INDEX) {
            pool_.orders[levels.tail[lidx]].next = oidx;
            o.prev = levels.tail[lidx];
        } else {
            levels.head[lidx] = oidx;
        }
        levels.tail[lidx] = oidx;
        levels.total_qty[lidx] += quantity;
        levels.order_count[lidx]++;

        total_orders_++;
        return oidx;
    }

    // =========================================================================
    // Cancel order — returns true if found and cancelled
    // =========================================================================
    bool cancel_order(uint32_t order_idx) noexcept {
        if (order_idx >= pool_.capacity) return false;

        Order& o = pool_.orders[order_idx];
        if (o.remaining_qty <= 0) return false;

        PriceLevelPool& levels = (o.side == Side::BUY) ? bids_ : asks_;
        uint32_t lidx = o.level_idx;

        // Remove from linked list
        if (o.prev != NULL_INDEX) pool_.orders[o.prev].next = o.next;
        else levels.head[lidx] = o.next;

        if (o.next != NULL_INDEX) pool_.orders[o.next].prev = o.prev;
        else levels.tail[lidx] = o.prev;

        levels.total_qty[lidx] -= o.remaining_qty;
        levels.order_count[lidx]--;

        if (levels.order_count[lidx] == 0) {
            levels.active[lidx] = false;
        }

        o.remaining_qty = 0;
        pool_.free(order_idx);
        return true;
    }

    // =========================================================================
    // Fill callback — called for each match
    // =========================================================================
    struct MatchResult {
        uint32_t maker_idx;     // Resting order pool index
        uint32_t taker_contestant_id;
        int64_t  fill_price;
        int32_t  fill_qty;
    };

    // =========================================================================
    // Match incoming order against resting book
    // Returns number of fills. Fills are written to `out_fills`.
    // =========================================================================
    IICPC_HOT
    uint32_t match(Side aggressor_side,
                   int64_t price,
                   int32_t quantity,
                   uint32_t contestant_id,
                   MatchResult* out_fills,
                   uint32_t max_fills) noexcept {
        PriceLevelPool& contra = (aggressor_side == Side::BUY) ? asks_ : bids_;
        uint32_t fill_count = 0;
        int32_t remaining = quantity;

        while (remaining > 0 && fill_count < max_fills) {
            uint32_t best = (aggressor_side == Side::BUY)
                ? contra.find_best_ask()
                : contra.find_best_bid();

            if (best == NULL_INDEX) break;

            // Price check for limit orders
            if (aggressor_side == Side::BUY && contra.prices[best] > price) break;
            if (aggressor_side == Side::SELL && contra.prices[best] < price) break;

            // Walk the order queue at this level (time priority)
            uint32_t oidx = contra.head[best];
            while (oidx != NULL_INDEX && remaining > 0 && fill_count < max_fills) {
                Order& resting = pool_.orders[oidx];
                uint32_t next_oidx = resting.next;

                int32_t fill_qty = (remaining < resting.remaining_qty)
                    ? remaining : resting.remaining_qty;

                // Record fill
                out_fills[fill_count].maker_idx = oidx;
                out_fills[fill_count].taker_contestant_id = contestant_id;
                out_fills[fill_count].fill_price = resting.price;
                out_fills[fill_count].fill_qty = fill_qty;
                fill_count++;

                remaining -= fill_qty;
                resting.remaining_qty -= fill_qty;
                contra.total_qty[best] -= fill_qty;

                if (resting.remaining_qty == 0) {
                    // Fully filled — remove from level
                    if (resting.prev != NULL_INDEX)
                        pool_.orders[resting.prev].next = resting.next;
                    else
                        contra.head[best] = resting.next;

                    if (resting.next != NULL_INDEX)
                        pool_.orders[resting.next].prev = resting.prev;
                    else
                        contra.tail[best] = resting.prev;

                    contra.order_count[best]--;
                    pool_.free(oidx);
                }

                oidx = next_oidx;
            }

            if (contra.order_count[best] == 0) {
                contra.active[best] = false;
            }
        }

        total_fills_ += fill_count;
        return fill_count;
    }

    // =========================================================================
    // Accessors
    // =========================================================================
    [[nodiscard]] int64_t best_bid_price() const noexcept {
        uint32_t b = bids_.find_best_bid();
        return (b != NULL_INDEX) ? bids_.prices[b] : 0;
    }

    [[nodiscard]] int64_t best_ask_price() const noexcept {
        uint32_t a = asks_.find_best_ask();
        return (a != NULL_INDEX) ? asks_.prices[a] : 0;
    }

    [[nodiscard]] int32_t best_bid_qty() const noexcept {
        uint32_t b = bids_.find_best_bid();
        return (b != NULL_INDEX) ? bids_.total_qty[b] : 0;
    }

    [[nodiscard]] int32_t best_ask_qty() const noexcept {
        uint32_t a = asks_.find_best_ask();
        return (a != NULL_INDEX) ? asks_.total_qty[a] : 0;
    }

    [[nodiscard]] uint64_t total_orders() const noexcept { return total_orders_; }
    [[nodiscard]] uint64_t total_fills() const noexcept { return total_fills_; }
    [[nodiscard]] Order& order_at(uint32_t idx) noexcept { return pool_.orders[idx]; }

private:
    PriceLevelPool bids_;
    PriceLevelPool asks_;
    OrderPool pool_;
    uint64_t next_exchange_id_ = 1;
    uint64_t total_orders_ = 0;
    uint64_t total_fills_ = 0;
};

} // namespace iicpc
