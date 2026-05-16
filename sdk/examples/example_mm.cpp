// =============================================================================
// example_mm.cpp — Example Market-Making Strategy
// =============================================================================
// Simple market-maker: quotes bid/ask around mid price with a configurable
// spread. Manages inventory with position-based skewing.
//
// This is the template contestants start from.
// =============================================================================

#include "sdk/strategy_sdk.hpp"
#include <cstdio>

namespace iicpc {

class MarketMakerStrategy final : public IStrategy {
public:
    void on_session_start(IOrderGateway& gw,
                          const SessionStart& session) noexcept override {
        gw_ = &gw;
        max_position_ = session.max_position;
        max_order_size_ = session.max_order_size;
        std::fprintf(stderr, "[mm] Session started. Max position: %d\n",
                     max_position_);
    }

    void on_market_data(const MarketUpdate& update) noexcept override {
        if (!gw_) return;
        tick_count_++;

        int64_t mid = (update.best_bid_price + update.best_ask_price) / 2;
        if (mid <= 0) return;

        int32_t pos = gw_->position();
        int64_t spread = (update.best_ask_price - update.best_bid_price);
        if (spread <= 0) spread = 100; // 1 cent minimum

        // Position-based skew: lean away from large inventory
        int64_t skew = static_cast<int64_t>(pos) * 10; // 0.001 per unit

        // Quote
        int64_t my_bid = mid - spread / 2 - skew;
        int64_t my_ask = mid + spread / 2 - skew;

        // Don't quote if at position limits
        int32_t quote_size = 10;
        if (quote_size > max_order_size_) quote_size = max_order_size_;

        // Cancel previous orders every N ticks (simple approach)
        if (tick_count_ % 5 == 0) {
            if (active_bid_id_ != 0) {
                CancelRequest cancel{};
                cancel.msg_type = MsgType::CANCEL_REQUEST;
                cancel.client_order_id = active_bid_id_;
                gw_->send_cancel(cancel);
                active_bid_id_ = 0;
            }
            if (active_ask_id_ != 0) {
                CancelRequest cancel{};
                cancel.msg_type = MsgType::CANCEL_REQUEST;
                cancel.client_order_id = active_ask_id_;
                gw_->send_cancel(cancel);
                active_ask_id_ = 0;
            }
        }

        // Place bid if not at limit
        if (pos < max_position_ && active_bid_id_ == 0) {
            OrderEntry bid{};
            bid.msg_type = MsgType::ORDER_ENTRY;
            bid.side = Side::BUY;
            bid.order_type = OrderType::LIMIT;
            bid.instrument_id = 0;
            bid.client_order_id = gw_->next_order_id();
            bid.price = my_bid;
            bid.quantity = quote_size;
            if (gw_->send_order(bid)) {
                active_bid_id_ = bid.client_order_id;
            }
        }

        // Place ask if not at limit
        if (pos > -max_position_ && active_ask_id_ == 0) {
            OrderEntry ask{};
            ask.msg_type = MsgType::ORDER_ENTRY;
            ask.side = Side::SELL;
            ask.order_type = OrderType::LIMIT;
            ask.instrument_id = 0;
            ask.client_order_id = gw_->next_order_id();
            ask.price = my_ask;
            ask.quantity = quote_size;
            if (gw_->send_order(ask)) {
                active_ask_id_ = ask.client_order_id;
            }
        }
    }

    void on_order_ack(const OrderAck& ack) noexcept override {
        if (ack.status != AckStatus::ACCEPTED) {
            // Rejected — clear our tracking
            if (ack.client_order_id == active_bid_id_) active_bid_id_ = 0;
            if (ack.client_order_id == active_ask_id_) active_ask_id_ = 0;
        }
    }

    void on_fill(const Fill& fill) noexcept override {
        fill_count_++;
        if (fill.remaining_qty == 0) {
            // Fully filled — clear tracking
            if (fill.client_order_id == active_bid_id_) active_bid_id_ = 0;
            if (fill.client_order_id == active_ask_id_) active_ask_id_ = 0;
        }
    }

    void on_cancel_ack(const CancelAck& ack) noexcept override {
        if (ack.client_order_id == active_bid_id_) active_bid_id_ = 0;
        if (ack.client_order_id == active_ask_id_) active_ask_id_ = 0;
    }

    void on_session_end(const SessionEnd& session) noexcept override {
        std::fprintf(stderr,
            "[mm] Session ended. PnL: %ld | Fills: %u | Position: %d\n",
            session.final_pnl, fill_count_, session.final_position);
    }

private:
    IOrderGateway* gw_ = nullptr;
    int32_t max_position_ = 100;
    int32_t max_order_size_ = 50;
    uint32_t tick_count_ = 0;
    uint32_t fill_count_ = 0;
    uint32_t active_bid_id_ = 0;
    uint32_t active_ask_id_ = 0;
};

// Factory function — required by the platform
IStrategy* create_strategy() {
    static MarketMakerStrategy instance;
    return &instance;
}

} // namespace iicpc
