// --- Example Orderbook Engine (Price-Time Priority) ---
// Reference limit orderbook implementation.
// Maintains bid/ask sides as sorted arrays. Matches aggressive orders
// against resting liquidity. Handles cancels.
// Serves as the reference implementation for shadow orderbook validation.

#include "sdk/strategy_sdk.hpp"
#include "sdk/protocol.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

using namespace iicpc;

// Simple price-time priority orderbook
struct RestingOrder {
    uint32_t order_id;
    int64_t  price;
    int32_t  quantity;
    Side     side;
    uint32_t sequence;  // Insertion order for time priority
};

class SimpleOrderbook final : public IStrategy {
public:
    void on_session_start(const SessionStart& session) noexcept override {
        bid_count_ = 0;
        ask_count_ = 0;
        seq_ = 0;
        std::fprintf(stderr, "[orderbook] Session started, ready for orders\n");
    }

    void on_order(const OrderEntry& order,
                  IResponseSender& sender) noexcept override {
        if (order.order_type == OrderType::MARKET) {
            process_market_order(order, sender);
        } else {
            process_limit_order(order, sender);
        }
    }

    void on_cancel(const CancelRequest& cancel,
                   IResponseSender& sender) noexcept override {
        ContestantResponse resp{};
        resp.order_id = cancel.client_order_id;
        resp.response_type = MsgType::CANCEL_ACK;

        // Search bids
        for (uint32_t i = 0; i < bid_count_; ++i) {
            if (bids_[i].order_id == cancel.client_order_id) {
                resp.ack_status = AckStatus::CANCELLED;
                resp.remaining_qty = bids_[i].quantity;
                remove_bid(i);
                sender.send(resp);
                return;
            }
        }

        // Search asks
        for (uint32_t i = 0; i < ask_count_; ++i) {
            if (asks_[i].order_id == cancel.client_order_id) {
                resp.ack_status = AckStatus::CANCELLED;
                resp.remaining_qty = asks_[i].quantity;
                remove_ask(i);
                sender.send(resp);
                return;
            }
        }

        // Not found — already filled or cancelled
        resp.ack_status = AckStatus::REJECTED_UNKNOWN;
        sender.send(resp);
    }

    void on_session_end(const SessionEnd& session) noexcept override {
        std::fprintf(stderr, "[orderbook] Session ended: "
                     "bids=%u asks=%u total_fills=%lu\n",
                     bid_count_, ask_count_, total_fills_);
    }

private:
    // Limit order: try to match, then rest
    void process_limit_order(const OrderEntry& order,
                             IResponseSender& sender) noexcept {
        int32_t remaining = order.quantity;

        if (order.side == Side::BUY) {
            // Buy order — match against asks (lowest price first)
            while (remaining > 0 && ask_count_ > 0 &&
                   order.price >= asks_[0].price) {
                int32_t fill_qty = std::min(remaining,
                                            asks_[0].quantity);
                // Send fill
                ContestantResponse fill{};
                fill.order_id = order.client_order_id;
                fill.response_type = MsgType::FILL;
                fill.ack_status = AckStatus::ACCEPTED;
                fill.fill_price = asks_[0].price;
                fill.fill_qty = fill_qty;
                fill.remaining_qty = remaining - fill_qty;
                fill.num_fills = 1;
                sender.send(fill);
                total_fills_++;

                remaining -= fill_qty;
                asks_[0].quantity -= fill_qty;
                if (asks_[0].quantity <= 0) {
                    remove_ask(0);
                }
            }
        } else {
            // Sell order — match against bids (highest price first)
            while (remaining > 0 && bid_count_ > 0 &&
                   order.price <= bids_[0].price) {
                int32_t fill_qty = std::min(remaining,
                                            bids_[0].quantity);
                // Send fill
                ContestantResponse fill{};
                fill.order_id = order.client_order_id;
                fill.response_type = MsgType::FILL;
                fill.ack_status = AckStatus::ACCEPTED;
                fill.fill_price = bids_[0].price;
                fill.fill_qty = fill_qty;
                fill.remaining_qty = remaining - fill_qty;
                fill.num_fills = 1;
                sender.send(fill);
                total_fills_++;

                remaining -= fill_qty;
                bids_[0].quantity -= fill_qty;
                if (bids_[0].quantity <= 0) {
                    remove_bid(0);
                }
            }
        }

        // Rest remaining quantity on the book
        if (remaining > 0) {
            insert_resting(order.client_order_id, order.price,
                          remaining, order.side);
        }

        // Send ACK
        ContestantResponse ack{};
        ack.order_id = order.client_order_id;
        ack.response_type = MsgType::ORDER_ACK;
        ack.ack_status = AckStatus::ACCEPTED;
        ack.remaining_qty = remaining;
        sender.send(ack);
    }

    // Market order: match immediately, no resting
    void process_market_order(const OrderEntry& order,
                              IResponseSender& sender) noexcept {
        int32_t remaining = order.quantity;

        if (order.side == Side::BUY) {
            while (remaining > 0 && ask_count_ > 0) {
                int32_t fill_qty = std::min(remaining,
                                            asks_[0].quantity);
                ContestantResponse fill{};
                fill.order_id = order.client_order_id;
                fill.response_type = MsgType::FILL;
                fill.ack_status = AckStatus::ACCEPTED;
                fill.fill_price = asks_[0].price;
                fill.fill_qty = fill_qty;
                fill.remaining_qty = remaining - fill_qty;
                fill.num_fills = 1;
                sender.send(fill);
                total_fills_++;

                remaining -= fill_qty;
                asks_[0].quantity -= fill_qty;
                if (asks_[0].quantity <= 0) remove_ask(0);
            }
        } else {
            while (remaining > 0 && bid_count_ > 0) {
                int32_t fill_qty = std::min(remaining,
                                            bids_[0].quantity);
                ContestantResponse fill{};
                fill.order_id = order.client_order_id;
                fill.response_type = MsgType::FILL;
                fill.ack_status = AckStatus::ACCEPTED;
                fill.fill_price = bids_[0].price;
                fill.fill_qty = fill_qty;
                fill.remaining_qty = remaining - fill_qty;
                fill.num_fills = 1;
                sender.send(fill);
                total_fills_++;

                remaining -= fill_qty;
                bids_[0].quantity -= fill_qty;
                if (bids_[0].quantity <= 0) remove_bid(0);
            }
        }

        // ACK (even if unfilled)
        ContestantResponse ack{};
        ack.order_id = order.client_order_id;
        ack.response_type = MsgType::ORDER_ACK;
        ack.ack_status = (remaining < order.quantity)
            ? AckStatus::ACCEPTED : AckStatus::REJECTED_PRICE;
        ack.remaining_qty = remaining;
        sender.send(ack);
    }

    // Book management
    void insert_resting(uint32_t id, int64_t price,
                        int32_t qty, Side side) noexcept {
        if (side == Side::BUY) {
            if (bid_count_ >= MAX_ORDERS) return;
            // Insert sorted: highest price first, then by sequence (FIFO)
            uint32_t pos = bid_count_;
            for (uint32_t i = 0; i < bid_count_; ++i) {
                if (price > bids_[i].price) { pos = i; break; }
                if (price == bids_[i].price && seq_ < bids_[i].sequence) {
                    pos = i; break;
                }
            }
            // Shift right
            if (pos < bid_count_) {
                std::memmove(&bids_[pos + 1], &bids_[pos],
                             sizeof(RestingOrder) * (bid_count_ - pos));
            }
            bids_[pos] = {id, price, qty, side, seq_++};
            bid_count_++;
        } else {
            if (ask_count_ >= MAX_ORDERS) return;
            // Insert sorted: lowest price first, then by sequence (FIFO)
            uint32_t pos = ask_count_;
            for (uint32_t i = 0; i < ask_count_; ++i) {
                if (price < asks_[i].price) { pos = i; break; }
                if (price == asks_[i].price && seq_ < asks_[i].sequence) {
                    pos = i; break;
                }
            }
            if (pos < ask_count_) {
                std::memmove(&asks_[pos + 1], &asks_[pos],
                             sizeof(RestingOrder) * (ask_count_ - pos));
            }
            asks_[pos] = {id, price, qty, side, seq_++};
            ask_count_++;
        }
    }

    void remove_bid(uint32_t idx) noexcept {
        if (idx < bid_count_ - 1) {
            std::memmove(&bids_[idx], &bids_[idx + 1],
                         sizeof(RestingOrder) * (bid_count_ - idx - 1));
        }
        bid_count_--;
    }

    void remove_ask(uint32_t idx) noexcept {
        if (idx < ask_count_ - 1) {
            std::memmove(&asks_[idx], &asks_[idx + 1],
                         sizeof(RestingOrder) * (ask_count_ - idx - 1));
        }
        ask_count_--;
    }

    static constexpr uint32_t MAX_ORDERS = 8192;

    RestingOrder bids_[MAX_ORDERS] = {};
    RestingOrder asks_[MAX_ORDERS] = {};
    uint32_t bid_count_ = 0;
    uint32_t ask_count_ = 0;
    uint32_t seq_ = 0;
    uint64_t total_fills_ = 0;
};

} // anonymous namespace

// Factory
iicpc::IStrategy* iicpc::create_strategy() {
    static SimpleOrderbook instance;
    return &instance;
}
