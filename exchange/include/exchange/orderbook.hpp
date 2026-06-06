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
#include <immintrin.h>

namespace iicpc {

// =============================================================================
// Constants
// =============================================================================
inline constexpr uint32_t MAX_ORDERS        = 65536;  // Per side
inline constexpr uint32_t MAX_PRICE_LEVELS_OB = 4096; // Distinct price levels per side
inline constexpr uint32_t NULL_INDEX        = UINT32_MAX;

// Hash map capacity: must be power-of-2 and >= 2x MAX_PRICE_LEVELS for <50% load factor
inline constexpr uint32_t PRICE_MAP_CAPACITY = 8192;
inline constexpr uint32_t PRICE_MAP_MASK     = PRICE_MAP_CAPACITY - 1;
static_assert((PRICE_MAP_CAPACITY & (PRICE_MAP_CAPACITY - 1)) == 0, "Must be power of 2");
static_assert(PRICE_MAP_CAPACITY >= MAX_PRICE_LEVELS_OB * 2, "Load factor must be < 0.5");

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
// Robin Hood Open-Addressed Hash Map: price → level index
// =============================================================================
// Fibonacci hashing gives excellent distribution for sequential integer keys.
// Robin Hood probing bounds worst-case probe length and keeps variance low.
// All memory from HugePageArena — zero heap allocation.
// =============================================================================
struct PriceHashMap {
    int64_t*  keys      = nullptr;  // Price keys (0 = empty sentinel)
    uint32_t* values    = nullptr;  // Level indices
    uint8_t*  distances = nullptr;  // Probe distance from ideal slot (Robin Hood)

    bool init(HugePageArena& arena) noexcept {
        keys      = arena.allocate<int64_t>(PRICE_MAP_CAPACITY);
        values    = arena.allocate<uint32_t>(PRICE_MAP_CAPACITY);
        distances = arena.allocate<uint8_t>(PRICE_MAP_CAPACITY);
        if (!keys || !values || !distances) return false;
        clear();
        return true;
    }

    void clear() noexcept {
        std::memset(keys, 0, sizeof(int64_t) * PRICE_MAP_CAPACITY);
        std::memset(values, 0xFF, sizeof(uint32_t) * PRICE_MAP_CAPACITY);  // NULL_INDEX
        std::memset(distances, 0, sizeof(uint8_t) * PRICE_MAP_CAPACITY);
    }

    // Fibonacci hash: golden ratio multiplicative hashing for int64 keys
    IICPC_FORCE_INLINE
    static uint32_t hash(int64_t price) noexcept {
        auto u = static_cast<uint64_t>(price);
        return static_cast<uint32_t>((u * 0x9E3779B97F4A7C15ULL) >> 51) & PRICE_MAP_MASK;
    }

    // O(1) amortized lookup with AVX2 4-way parallel key comparison.
    // Instead of comparing keys one-by-one, we load 4 consecutive keys into
    // a 256-bit AVX2 register and compare them all simultaneously.
    // This reduces the average probe cost from ~2-3 sequential comparisons
    // to a single SIMD instruction + bitmask extraction.
    [[nodiscard]] IICPC_HOT IICPC_AVX2_TARGET
    uint32_t find(int64_t price) const noexcept {
        uint32_t slot = hash(price);
        uint8_t dist = 0;

        // AVX2 fast path: probe 4 slots at once when alignment permits
        // and we have enough contiguous slots before wrap-around
        while (slot + 4 <= PRICE_MAP_CAPACITY) {
            // Prefetch next probe group while processing this one
            prefetch_read_l1(&keys[(slot + 4) & PRICE_MAP_MASK]);

            // Load 4 keys and compare with AVX2
            uint32_t mask = avx2_find_key_i64(&keys[slot], price);
            if (mask != 0) {
                // Found! Extract which of the 4 slots matched
                uint32_t offset = avx2_first_match_index_i64(mask);
                return values[slot + offset];
            }

            // Check for empty slot or Robin Hood termination in this group
            for (uint32_t i = 0; i < 4; ++i) {
                uint32_t s = slot + i;
                if (distances[s] == 0 && keys[s] == 0) return NULL_INDEX;
                if (dist + i > distances[s]) return NULL_INDEX;
            }

            slot = (slot + 4) & PRICE_MAP_MASK;
            dist += 4;
        }

        // Scalar fallback for wrap-around region
        while (true) {
            if (distances[slot] == 0 && keys[slot] == 0) return NULL_INDEX;
            if (dist > distances[slot]) return NULL_INDEX;
            if (keys[slot] == price) return values[slot];
            slot = (slot + 1) & PRICE_MAP_MASK;
            ++dist;
        }
    }

    // O(1) amortized insert. Assumes key doesn't already exist.
    void insert(int64_t price, uint32_t level_idx) noexcept {
        uint32_t slot = hash(price);
        int64_t cur_key = price;
        uint32_t cur_val = level_idx;
        uint8_t dist = 1;  // Start at 1 (0 = empty sentinel)

        while (true) {
            if (distances[slot] == 0 && keys[slot] == 0) {
                // Empty slot — place here
                keys[slot] = cur_key;
                values[slot] = cur_val;
                distances[slot] = dist;
                return;
            }
            if (distances[slot] < dist) {
                // Robin Hood: steal from the rich (shorter probe → displaced)
                std::swap(cur_key, keys[slot]);
                std::swap(cur_val, values[slot]);
                std::swap(dist, distances[slot]);
            }
            slot = (slot + 1) & PRICE_MAP_MASK;
            ++dist;
        }
    }

    // O(1) amortized removal with backward-shift deletion.
    void remove(int64_t price) noexcept {
        uint32_t slot = hash(price);
        while (true) {
            if (keys[slot] == price) break;
            if (distances[slot] == 0 && keys[slot] == 0) return;  // Not found
            slot = (slot + 1) & PRICE_MAP_MASK;
        }
        // Backward-shift deletion to maintain Robin Hood invariant
        while (true) {
            uint32_t next = (slot + 1) & PRICE_MAP_MASK;
            if (distances[next] <= 1) {
                // Next slot is empty or at its ideal position — stop
                keys[slot] = 0;
                values[slot] = NULL_INDEX;
                distances[slot] = 0;
                return;
            }
            // Shift next entry backward
            keys[slot] = keys[next];
            values[slot] = values[next];
            distances[slot] = distances[next] - 1;
            slot = next;
        }
    }
};

// =============================================================================
// Price Level Pool (SoA arrays) with O(1) hash-based lookup
// =============================================================================
struct PriceLevelPool {
    // SoA arrays — each field is a contiguous array
    int64_t*  prices       = nullptr; // Price at this level
    int32_t*  total_qty    = nullptr; // Total quantity at level
    uint32_t* order_count  = nullptr; // Number of orders at level
    uint32_t* head         = nullptr; // Head of order linked list
    uint32_t* tail         = nullptr; // Tail of order linked list
    bool*     active       = nullptr; // Is this level active?

    uint32_t  count        = 0;       // Current number of allocated levels
    uint32_t  capacity     = 0;

    // O(1) price → level index lookup
    PriceHashMap price_map;

    // Cached best price index — avoids O(N) scan on every match()
    uint32_t best_idx      = NULL_INDEX;

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
        if (!price_map.init(arena)) return false;

        std::memset(prices, 0, sizeof(int64_t) * cap);
        std::memset(total_qty, 0, sizeof(int32_t) * cap);
        std::memset(order_count, 0, sizeof(uint32_t) * cap);
        std::memset(head, 0xFF, sizeof(uint32_t) * cap);  // NULL_INDEX
        std::memset(tail, 0xFF, sizeof(uint32_t) * cap);
        std::memset(active, 0, sizeof(bool) * cap);
        return true;
    }

    // O(1) amortized: find or create a level for this price.
    uint32_t find_or_create(int64_t price) noexcept {
        // O(1) hash lookup
        uint32_t existing = price_map.find(price);
        if (existing != NULL_INDEX && active[existing]) return existing;

        // Create new level
        if (count >= capacity) return NULL_INDEX;
        uint32_t idx = count++;
        prices[idx] = price;
        total_qty[idx] = 0;
        order_count[idx] = 0;
        head[idx] = NULL_INDEX;
        tail[idx] = NULL_INDEX;
        active[idx] = true;
        price_map.insert(price, idx);
        return idx;
    }

    // O(1) cached best bid — highest price with quantity
    uint32_t find_best_bid() const noexcept {
        if (best_idx != NULL_INDEX && active[best_idx] && total_qty[best_idx] > 0)
            return best_idx;
        // Cache miss: rebuild (rare — only after level exhaustion)
        return rebuild_best_bid();
    }

    // O(1) cached best ask — lowest price with quantity
    uint32_t find_best_ask() const noexcept {
        if (best_idx != NULL_INDEX && active[best_idx] && total_qty[best_idx] > 0)
            return best_idx;
        return rebuild_best_ask();
    }

    // Invalidate cached best (called when level is exhausted or deactivated)
    void invalidate_best() noexcept { best_idx = NULL_INDEX; }

    // Update cached best after insert (BID side: want highest price)
    void update_best_bid(uint32_t new_idx) noexcept {
        if (best_idx == NULL_INDEX || prices[new_idx] > prices[best_idx])
            best_idx = new_idx;
    }

    // Update cached best after insert (ASK side: want lowest price)
    void update_best_ask(uint32_t new_idx) noexcept {
        if (best_idx == NULL_INDEX || prices[new_idx] < prices[best_idx])
            best_idx = new_idx;
    }

    // Deactivate a level — removes from hash map and invalidates best cache
    void deactivate(uint32_t idx) noexcept {
        active[idx] = false;
        price_map.remove(prices[idx]);
        if (best_idx == idx) best_idx = NULL_INDEX;
    }

private:
    // Fallback O(N) scan — only called when cached best is stale.
    // Uses AVX2 prefetching to hide latency during the linear scan.
    uint32_t rebuild_best_bid() const noexcept {
        uint32_t best = NULL_INDEX;
        int64_t best_price = INT64_MIN;
        for (uint32_t i = 0; i < count; ++i) {
            // Prefetch ahead in the SoA arrays to hide L2 latency
            if (IICPC_LIKELY(i + 8 < count)) {
                prefetch_read_l1(&prices[i + 8]);
                prefetch_read_l1(&active[i + 8]);
                prefetch_read_l1(&total_qty[i + 8]);
            }
            if (active[i] && total_qty[i] > 0 && prices[i] > best_price) {
                best_price = prices[i];
                best = i;
            }
        }
        const_cast<PriceLevelPool*>(this)->best_idx = best;
        return best;
    }

    uint32_t rebuild_best_ask() const noexcept {
        uint32_t best = NULL_INDEX;
        int64_t best_price = INT64_MAX;
        for (uint32_t i = 0; i < count; ++i) {
            if (IICPC_LIKELY(i + 8 < count)) {
                prefetch_read_l1(&prices[i + 8]);
                prefetch_read_l1(&active[i + 8]);
                prefetch_read_l1(&total_qty[i + 8]);
            }
            if (active[i] && total_qty[i] > 0 && prices[i] < best_price) {
                best_price = prices[i];
                best = i;
            }
        }
        const_cast<PriceLevelPool*>(this)->best_idx = best;
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

        // Update cached best price
        if (side == Side::BUY) levels.update_best_bid(lidx);
        else                   levels.update_best_ask(lidx);

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
            levels.deactivate(lidx);
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
    IICPC_HOT IICPC_FLATTEN
    uint32_t match(Side aggressor_side,
                   int64_t price,
                   int32_t quantity,
                   uint32_t contestant_id,
                   MatchResult* IICPC_RESTRICT out_fills,
                   uint32_t max_fills) noexcept {
        PriceLevelPool& contra = (aggressor_side == Side::BUY) ? asks_ : bids_;
        uint32_t fill_count = 0;
        int32_t remaining = quantity;

        // Prefetch the fills output array into L1 (we'll be writing to it)
        prefetch_write_l1(out_fills);

        while (IICPC_LIKELY(remaining > 0) && fill_count < max_fills) {
            uint32_t best = (aggressor_side == Side::BUY)
                ? contra.find_best_ask()
                : contra.find_best_bid();

            if (IICPC_UNLIKELY(best == NULL_INDEX)) break;

            // Price check for limit orders
            if (aggressor_side == Side::BUY && contra.prices[best] > price) break;
            if (aggressor_side == Side::SELL && contra.prices[best] < price) break;

            // Prefetch the head order at this level into L1 cache
            // This hides the BRAM-to-register latency (~12 cycles on L2 miss)
            uint32_t oidx = contra.head[best];
            if (IICPC_LIKELY(oidx != NULL_INDEX)) {
                prefetch_read_l1(&pool_.orders[oidx]);
            }

            // Walk the order queue at this level (time priority)
            while (oidx != NULL_INDEX && remaining > 0 && fill_count < max_fills) {
                Order& resting = pool_.orders[oidx];
                uint32_t next_oidx = resting.next;

                // Prefetch NEXT order while processing current (pipelined memory access)
                if (IICPC_LIKELY(next_oidx != NULL_INDEX)) {
                    prefetch_read_l1(&pool_.orders[next_oidx]);
                }

                // Prefetch next fill output slot
                if (IICPC_LIKELY(fill_count + 1 < max_fills)) {
                    prefetch_write_l1(&out_fills[fill_count + 1]);
                }

                int32_t fill_qty = (remaining < resting.remaining_qty)
                    ? remaining : resting.remaining_qty;

                // Record fill — use SIMD copy for the cache-line-aligned MatchResult
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
                contra.deactivate(best);
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
