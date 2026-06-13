#pragma once
// match_engine.hpp — Exchange Matching Engine
// Ties together the orderbook, market data, and gateway I/O.
// Runs on a pinned core, processes orders, generates fills/acks,
// publishes market data to connected gateways.
//
// Single instrument. PnL-first scoring.

#include "exchange/orderbook.hpp"
#include "exchange/market_data_gen.hpp"
#include "sdk/protocol.hpp"
#include "core/arena.hpp"
#include "core/compiler_hints.hpp"

#include <cstdint>
#include <cstdio>
#include <chrono>

namespace iicpc {

// Per-contestant state (SoA, tracked by engine)
inline constexpr uint32_t MAX_CONTESTANTS = 64;
inline constexpr uint32_t MAX_ACTIVE_ORDERS_PER_CONTESTANT = 256;

struct ContestantState {
    // SoA arrays
    int32_t*  position      = nullptr; // Current position per contestant
    int64_t*  realized_pnl  = nullptr; // Realized PnL
    int64_t*  avg_entry     = nullptr; // Average entry price
    int64_t*  cash          = nullptr; // Cash balance
    uint32_t* order_count   = nullptr; // Active order count
    uint32_t* total_fills   = nullptr; // Total fill count
    uint32_t* total_orders  = nullptr; // Total orders submitted
    uint64_t* last_heartbeat = nullptr; // Last heartbeat timestamp
    bool*     connected     = nullptr; // Is contestant connected?
    uint32_t  count         = 0;

    bool init(HugePageArena& arena, uint32_t max_c) noexcept {
        position       = arena.allocate<int32_t>(max_c);
        realized_pnl   = arena.allocate<int64_t>(max_c);
        avg_entry      = arena.allocate<int64_t>(max_c);
        cash           = arena.allocate<int64_t>(max_c);
        order_count    = arena.allocate<uint32_t>(max_c);
        total_fills    = arena.allocate<uint32_t>(max_c);
        total_orders   = arena.allocate<uint32_t>(max_c);
        last_heartbeat = arena.allocate<uint64_t>(max_c);
        connected      = arena.allocate<bool>(max_c);

        if (!position || !realized_pnl || !avg_entry || !cash ||
            !order_count || !total_fills || !total_orders ||
            !last_heartbeat || !connected) return false;

        std::memset(position, 0, sizeof(int32_t) * max_c);
        std::memset(realized_pnl, 0, sizeof(int64_t) * max_c);
        std::memset(avg_entry, 0, sizeof(int64_t) * max_c);
        std::memset(cash, 0, sizeof(int64_t) * max_c);
        std::memset(order_count, 0, sizeof(uint32_t) * max_c);
        std::memset(total_fills, 0, sizeof(uint32_t) * max_c);
        std::memset(total_orders, 0, sizeof(uint32_t) * max_c);
        std::memset(last_heartbeat, 0, sizeof(uint64_t) * max_c);
        std::memset(connected, 0, sizeof(bool) * max_c);
        return true;
    }

    /// Update PnL after a fill
    void apply_fill(uint32_t cid, Side side, int64_t price,
                    int32_t qty) noexcept {
        int32_t signed_qty = (side == Side::BUY) ? qty : -qty;
        int32_t old_pos = position[cid];

        // PnL calculation
        if ((old_pos > 0 && signed_qty < 0) ||
            (old_pos < 0 && signed_qty > 0)) {
            // Closing — realize PnL
            int32_t close_qty = (signed_qty > 0)
                ? std::min(signed_qty, -old_pos)
                : std::max(signed_qty, -old_pos);
            int32_t abs_close = (close_qty >= 0) ? close_qty : -close_qty;

            if (old_pos > 0) { // Was long, selling
                realized_pnl[cid] += static_cast<int64_t>(abs_close) *
                    (price - avg_entry[cid]);
            } else { // Was short, buying
                realized_pnl[cid] += static_cast<int64_t>(abs_close) *
                    (avg_entry[cid] - price);
            }
        }

        // Update average entry for opening/extending
        if ((old_pos >= 0 && signed_qty > 0) ||
            (old_pos <= 0 && signed_qty < 0)) {
            int32_t abs_old = (old_pos >= 0) ? old_pos : -old_pos;
            int32_t abs_new = (signed_qty >= 0) ? signed_qty : -signed_qty;
            int64_t old_cost = avg_entry[cid] * abs_old;
            int64_t new_cost = price * abs_new;
            int32_t total = abs_old + abs_new;
            if (total > 0) avg_entry[cid] = (old_cost + new_cost) / total;
        }

        position[cid] += signed_qty;
        total_fills[cid]++;
    }

    /// Mark-to-market unrealized PnL
    [[nodiscard]] int64_t unrealized_pnl(uint32_t cid,
                                          int64_t mid_price) const noexcept {
        if (position[cid] == 0) return 0;
        if (position[cid] > 0) {
            return static_cast<int64_t>(position[cid]) *
                (mid_price - avg_entry[cid]);
        } else {
            return static_cast<int64_t>(-position[cid]) *
                (avg_entry[cid] - mid_price);
        }
    }

    [[nodiscard]] int64_t total_pnl(uint32_t cid,
                                     int64_t mid_price) const noexcept {
        return realized_pnl[cid] + unrealized_pnl(cid, mid_price);
    }
};

// Match Engine Configuration
struct MatchEngineConfig {
    uint32_t tick_interval_us   = 100;    // Microseconds between ticks
    uint32_t match_duration_ms  = 30000;  // 30 seconds
    int32_t  max_position       = 100;    // Per-contestant max position
    int32_t  max_order_size     = 50;     // Max single order size
    int32_t  max_orders_per_sec = 1000;   // Rate limit
    int64_t  initial_cash       = 10000000; // $1000.00 starting cash
    MarketDataConfig market_cfg{};
};

// Outbound message queue (engine → gateway)
// Each contestant has a queue of outbound messages.
inline constexpr uint32_t OUTBOUND_QUEUE_SIZE = 4096;

struct OutboundMsg {
    alignas(64) uint8_t data[64]; // Largest message = 64 bytes
    uint32_t size;
    uint32_t contestant_id;
};

// Match Engine
class MatchEngine {
public:
    MatchEngine() noexcept = default;

    bool init(HugePageArena& arena, const MatchEngineConfig& cfg) noexcept {
        cfg_ = cfg;
        if (!book_.init(arena)) return false;
        if (!contestants_.init(arena, MAX_CONTESTANTS)) return false;
        market_gen_.init(cfg.market_cfg);

        // Outbound message ring
        outbound_ = static_cast<OutboundMsg*>(
            arena.allocate_raw(sizeof(OutboundMsg) * OUTBOUND_QUEUE_SIZE,
                               CACHE_LINE_SIZE));
        if (!outbound_) return false;
        outbound_head_ = 0;
        outbound_tail_ = 0;

        // O(1) cancel lookup: client_order_id → book pool index
        // Open-addressing hash map with power-of-2 size
        cancel_map_keys_ = arena.allocate<uint64_t>(CANCEL_MAP_SIZE);
        cancel_map_vals_ = arena.allocate<uint32_t>(CANCEL_MAP_SIZE);
        if (!cancel_map_keys_ || !cancel_map_vals_) return false;
        std::memset(cancel_map_keys_, 0, sizeof(uint64_t) * CANCEL_MAP_SIZE);
        std::memset(cancel_map_vals_, 0xFF, sizeof(uint32_t) * CANCEL_MAP_SIZE); // NULL_INDEX

        return true;
    }

    /// Register a contestant. Returns contestant ID.
    uint32_t register_contestant(int64_t initial_cash) noexcept {
        uint32_t cid = contestants_.count++;
        contestants_.cash[cid] = initial_cash;
        contestants_.connected[cid] = true;
        return cid;
    }

    // Process an incoming order from a contestant
    IICPC_HOT
    void process_order(uint32_t contestant_id,
                       const OrderEntry& order,
                       uint64_t timestamp_ns) noexcept {
        // Validate
        if (contestant_id >= contestants_.count) return;
        if (order.quantity <= 0 || order.quantity > cfg_.max_order_size) {
            send_ack(contestant_id, order.client_order_id,
                     AckStatus::REJECTED_SIZE, 0, order.price, order.quantity);
            return;
        }

        // Position limit check
        int32_t pos = contestants_.position[contestant_id];
        if (order.side == Side::BUY && pos + order.quantity > cfg_.max_position) {
            send_ack(contestant_id, order.client_order_id,
                     AckStatus::REJECTED_RISK, 0, order.price, order.quantity);
            return;
        }
        if (order.side == Side::SELL && pos - order.quantity < -cfg_.max_position) {
            send_ack(contestant_id, order.client_order_id,
                     AckStatus::REJECTED_RISK, 0, order.price, order.quantity);
            return;
        }

        contestants_.total_orders[contestant_id]++;

        // Try to match against the book
        OrderBook::MatchResult fills[64];
        uint32_t nfills = book_.match(order.side, order.price,
                                       order.quantity, contestant_id,
                                       fills, 64);

        int32_t filled_qty = 0;
        for (uint32_t i = 0; i < nfills; ++i) {
            auto& f = fills[i];
            Order& maker = book_.order_at(f.maker_idx);

            // Update maker PnL
            contestants_.apply_fill(maker.contestant_id,
                                    maker.side, f.fill_price, f.fill_qty);

            // Update taker (aggressor) PnL
            contestants_.apply_fill(contestant_id,
                                    order.side, f.fill_price, f.fill_qty);

            // Send fill to maker
            send_fill(maker.contestant_id, maker.client_order_id,
                      maker.exchange_id, maker.side, f.fill_price,
                      f.fill_qty, maker.remaining_qty, timestamp_ns);

            // Send fill to taker
            send_fill(contestant_id, order.client_order_id,
                      0, order.side, f.fill_price, f.fill_qty,
                      order.quantity - filled_qty - f.fill_qty, timestamp_ns);

            filled_qty += f.fill_qty;

            // Update last trade
            last_trade_price_ = f.fill_price;
            last_trade_qty_ = f.fill_qty;
        }

        // Remaining quantity rests in the book
        int32_t remaining = order.quantity - filled_qty;
        if (remaining > 0 && order.order_type == OrderType::LIMIT) {
            uint32_t oidx = book_.add_order(contestant_id,
                order.client_order_id, order.side, order.price,
                remaining, timestamp_ns);

            if (oidx != NULL_INDEX) {
                Order& o = book_.order_at(oidx);
                send_ack(contestant_id, order.client_order_id,
                         AckStatus::ACCEPTED, o.exchange_id,
                         order.price, remaining);
                contestants_.order_count[contestant_id]++;

                // Register in O(1) cancel lookup
                cancel_map_insert(make_cancel_key(contestant_id,
                                                  order.client_order_id), oidx);
            }
        } else if (filled_qty > 0) {
            // Fully filled — ack with accepted
            send_ack(contestant_id, order.client_order_id,
                     AckStatus::ACCEPTED, 0, order.price, filled_qty);
        }
    }

    // Process cancel from contestant
    void process_cancel(uint32_t contestant_id,
                        const CancelRequest& cancel,
                        uint64_t timestamp_ns) noexcept {
        CancelAck ack{};
        ack.msg_type = MsgType::CANCEL_ACK;
        ack.client_order_id = cancel.client_order_id;
        ack.exchange_ts_ns = timestamp_ns;

        // O(1) cancel lookup via hash map
        uint64_t lookup_key = make_cancel_key(contestant_id, cancel.client_order_id);
        uint32_t order_idx = cancel_map_lookup(lookup_key);

        if (order_idx == NULL_INDEX) {
            ack.status = AckStatus::REJECTED_UNKNOWN;
        } else {
            // Verify ownership
            Order& o = book_.order_at(order_idx);
            if (o.contestant_id != contestant_id || o.remaining_qty <= 0) {
                ack.status = AckStatus::REJECTED_UNKNOWN;
            } else {
                // Cancel the order
                int32_t cancelled_qty = o.remaining_qty;
                book_.cancel_order(order_idx);
                cancel_map_remove(lookup_key);
                contestants_.order_count[contestant_id]--;
                ack.status = AckStatus::CANCELLED;
            }
        }

        enqueue_outbound(contestant_id, &ack, sizeof(ack));
    }

    // Generate next market data tick
    IICPC_HOT
    MarketUpdate generate_tick(uint64_t timestamp_ns) noexcept {
        MarketUpdate update = market_gen_.next_tick(timestamp_ns);

        // Override with actual book state if we have orders
        int64_t bb = book_.best_bid_price();
        int64_t ba = book_.best_ask_price();
        if (bb > 0) update.best_bid_price = bb;
        if (ba > 0) update.best_ask_price = ba;
        if (bb > 0) update.best_bid_qty = book_.best_bid_qty();
        if (ba > 0) update.best_ask_qty = book_.best_ask_qty();
        if (last_trade_price_ > 0) {
            update.last_trade_price = last_trade_price_;
            update.last_trade_qty = last_trade_qty_;
        }

        tick_count_++;
        return update;
    }

    // Pop outbound message for a contestant
    bool pop_outbound(OutboundMsg& msg) noexcept {
        if (outbound_head_ == outbound_tail_) return false;
        msg = outbound_[outbound_head_ % OUTBOUND_QUEUE_SIZE];
        outbound_head_++;
        return true;
    }

    // Generate session end for a contestant
    SessionEnd make_session_end(uint32_t cid, uint64_t ts) const noexcept {
        int64_t mid = (book_.best_bid_price() + book_.best_ask_price()) / 2;
        if (mid <= 0) mid = market_gen_.last_update().last_trade_price;

        SessionEnd end{};
        end.msg_type = MsgType::SESSION_END;
        end.total_fills = contestants_.total_fills[cid];
        end.final_pnl = contestants_.total_pnl(cid, mid);
        end.final_position = contestants_.position[cid];
        end.total_orders = contestants_.total_orders[cid];
        end.end_timestamp_ns = ts;
        return end;
    }

    // Accessors
    [[nodiscard]] const ContestantState& contestants() const noexcept {
        return contestants_;
    }
    [[nodiscard]] uint64_t tick_count() const noexcept { return tick_count_; }
    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
    [[nodiscard]] const MatchEngineConfig& config() const noexcept { return cfg_; }

private:
    void send_ack(uint32_t cid, uint32_t client_oid, AckStatus status,
                  uint64_t exchange_oid, int64_t price, int32_t qty) noexcept {
        OrderAck ack{};
        ack.msg_type = MsgType::ORDER_ACK;
        ack.status = status;
        ack.client_order_id = client_oid;
        ack.exchange_order_id = exchange_oid;
        ack.price = price;
        ack.quantity = qty;
        enqueue_outbound(cid, &ack, sizeof(ack));
    }

    void send_fill(uint32_t cid, uint32_t client_oid, uint64_t exchange_oid,
                   Side side, int64_t price, int32_t qty, int32_t remaining,
                   uint64_t ts) noexcept {
        int64_t mid = (book_.best_bid_price() + book_.best_ask_price()) / 2;

        Fill fill{};
        fill.msg_type = MsgType::FILL;
        fill.side = side;
        fill.instrument_id = 0;
        fill.client_order_id = client_oid;
        fill.exchange_order_id = exchange_oid;
        fill.fill_price = price;
        fill.fill_qty = qty;
        fill.remaining_qty = remaining;
        fill.realized_pnl = contestants_.realized_pnl[cid];
        fill.exchange_ts_ns = ts;
        enqueue_outbound(cid, &fill, sizeof(fill));
    }

    void enqueue_outbound(uint32_t cid, const void* data,
                          uint32_t size) noexcept {
        uint32_t idx = outbound_tail_ % OUTBOUND_QUEUE_SIZE;
        std::memcpy(outbound_[idx].data, data, size);
        outbound_[idx].size = size;
        outbound_[idx].contestant_id = cid;
        outbound_tail_++;
    }

    // O(1) cancel map helpers
    static constexpr uint32_t CANCEL_MAP_SIZE = 65536; // Power of 2
    static constexpr uint32_t CANCEL_MAP_MASK = CANCEL_MAP_SIZE - 1;

    static uint64_t make_cancel_key(uint32_t cid, uint32_t client_oid) noexcept {
        return (static_cast<uint64_t>(cid) << 32) | client_oid;
    }

    void cancel_map_insert(uint64_t key, uint32_t pool_idx) noexcept {
        uint32_t slot = static_cast<uint32_t>(key * 0x9E3779B97F4A7C15ULL >> 48) & CANCEL_MAP_MASK;
        for (uint32_t i = 0; i < 64; ++i) { // Linear probing, max 64 steps
            uint32_t idx = (slot + i) & CANCEL_MAP_MASK;
            if (cancel_map_keys_[idx] == 0 || cancel_map_vals_[idx] == NULL_INDEX) {
                cancel_map_keys_[idx] = key;
                cancel_map_vals_[idx] = pool_idx;
                return;
            }
        }
    }

    uint32_t cancel_map_lookup(uint64_t key) const noexcept {
        uint32_t slot = static_cast<uint32_t>(key * 0x9E3779B97F4A7C15ULL >> 48) & CANCEL_MAP_MASK;
        for (uint32_t i = 0; i < 64; ++i) {
            uint32_t idx = (slot + i) & CANCEL_MAP_MASK;
            if (cancel_map_keys_[idx] == key && cancel_map_vals_[idx] != NULL_INDEX)
                return cancel_map_vals_[idx];
            if (cancel_map_keys_[idx] == 0) return NULL_INDEX;
        }
        return NULL_INDEX;
    }

    void cancel_map_remove(uint64_t key) noexcept {
        uint32_t slot = static_cast<uint32_t>(key * 0x9E3779B97F4A7C15ULL >> 48) & CANCEL_MAP_MASK;
        for (uint32_t i = 0; i < 64; ++i) {
            uint32_t idx = (slot + i) & CANCEL_MAP_MASK;
            if (cancel_map_keys_[idx] == key) {
                cancel_map_vals_[idx] = NULL_INDEX;
                return;
            }
            if (cancel_map_keys_[idx] == 0) return;
        }
    }

    MatchEngineConfig cfg_{};
    OrderBook book_;
    ContestantState contestants_;
    MarketDataGenerator market_gen_;
    OutboundMsg* outbound_ = nullptr;
    uint32_t outbound_head_ = 0;
    uint32_t outbound_tail_ = 0;
    int64_t last_trade_price_ = 0;
    int32_t last_trade_qty_ = 0;
    uint64_t tick_count_ = 0;

    // O(1) cancel lookup table
    uint64_t* cancel_map_keys_ = nullptr;
    uint32_t* cancel_map_vals_ = nullptr;
};

} // namespace iicpc
