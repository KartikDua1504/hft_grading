#pragma once

// --- Deterministic Reference Orderbook for Validation ---
// Processes the same order stream as the contestant.
// Produces expected fills; diffs against contestant's output.
// Runs on host side, independent of contestant VM.

#include "exchange/orderbook.hpp"
#include "sdk/protocol.hpp"
#include "core/arena.hpp"
#include "core/hot_path_asm.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace iicpc {

// --- Expected Fill Record ---
struct alignas(32) ExpectedFill {
    uint32_t order_id;
    int64_t  fill_price;
    int32_t  fill_qty;
    Side     aggressor_side;
    uint8_t  _pad[11];
};
static_assert(sizeof(ExpectedFill) == 32);

// --- Validation Result ---
struct ValidationResult {
    uint64_t total_orders      = 0;
    uint64_t total_expected    = 0;
    uint64_t total_actual      = 0;
    uint64_t correct_fills     = 0;
    uint64_t wrong_price       = 0;
    uint64_t wrong_qty         = 0;
    uint64_t missing_fills     = 0;
    uint64_t extra_fills       = 0;
    uint64_t priority_errors   = 0;
    uint64_t total_acks        = 0;

    [[nodiscard]] double correctness_score() const noexcept {
        if (total_expected == 0) return 1.0;
        double base = static_cast<double>(correct_fills) /
                      static_cast<double>(total_expected);
        double penalty = static_cast<double>(priority_errors) * 0.1;
        if (total_expected > 0) {
            double extra_ratio = static_cast<double>(extra_fills) /
                                 static_cast<double>(total_expected + extra_fills);
            double missing_ratio = static_cast<double>(missing_fills) /
                                   static_cast<double>(total_expected);
            penalty += extra_ratio * 0.3 + missing_ratio * 0.3;
        }
        double score = base - penalty;
        return (score < 0.0) ? 0.0 : ((score > 1.0) ? 1.0 : score);
    }
};

// --- Shadow Orderbook ---
inline constexpr uint32_t SHADOW_MAX_FILLS = 262144;

class ShadowOrderbook {
public:
    ShadowOrderbook() noexcept = default;

    bool init(HugePageArena& arena) noexcept {
        if (!book_.init(arena)) return false;
        expected_ = arena.allocate<ExpectedFill>(SHADOW_MAX_FILLS);
        if (!expected_) return false;
        expected_count_ = 0;
        return true;
    }

    void process_order(const OrderEntry& order) noexcept {
        result_.total_orders++;
        if (order.order_type == OrderType::LIMIT ||
            order.order_type == OrderType::MARKET ||
            order.order_type == OrderType::IOC) {

            int64_t match_price = order.price;
            if (order.order_type == OrderType::MARKET) {
                match_price = (order.side == Side::BUY) ? INT64_MAX : 0;
            }

            OrderBook::MatchResult fills[64];
            uint32_t nfills = book_.match(
                order.side, match_price, order.quantity, 0, fills, 64);

            for (uint32_t i = 0; i < nfills; ++i) {
                if (expected_count_ < SHADOW_MAX_FILLS) {
                    ExpectedFill& ef = expected_[expected_count_++];
                    ef.order_id = order.client_order_id;
                    ef.fill_price = fills[i].fill_price;
                    ef.fill_qty = fills[i].fill_qty;
                    ef.aggressor_side = order.side;
                    result_.total_expected++;
                }
            }

            int32_t filled = 0;
            for (uint32_t i = 0; i < nfills; ++i) filled += fills[i].fill_qty;
            int32_t remaining = order.quantity - filled;

            if (remaining > 0 && order.order_type == OrderType::LIMIT) {
                uint64_t ts = asm_hot::rdtsc_serialized();
                book_.add_order(0, order.client_order_id, order.side,
                               order.price, remaining, ts);
            }
        }
    }

    void process_cancel(const CancelRequest& cancel) noexcept {
        (void)cancel;
    }

    void validate_fill(uint32_t order_id, int64_t price, int32_t qty) noexcept {
        result_.total_actual++;
        bool found = false;
        for (uint32_t i = 0; i < expected_count_; ++i) {
            ExpectedFill& ef = expected_[i];
            if (ef.order_id == order_id && ef.fill_qty > 0) {
                if (ef.fill_price == price && ef.fill_qty == qty) {
                    result_.correct_fills++;
                } else if (ef.fill_price != price) {
                    result_.wrong_price++;
                } else {
                    result_.wrong_qty++;
                }
                ef.fill_qty = 0;
                found = true;
                break;
            }
        }
        if (!found) result_.extra_fills++;
    }

    void finalize() noexcept {
        for (uint32_t i = 0; i < expected_count_; ++i) {
            if (expected_[i].fill_qty > 0) result_.missing_fills++;
        }
    }

    [[nodiscard]] const ValidationResult& result() const noexcept { return result_; }
    [[nodiscard]] ValidationResult& result_mut() noexcept { return result_; }

    void print_report() const noexcept {
        std::fprintf(stderr,
            "\n--- Correctness Validation Report ---\n"
            "  Total Orders:       %lu\n"
            "  Expected Fills:     %lu\n"
            "  Actual Fills:       %lu\n"
            "  Correct:            %lu\n"
            "  Wrong Price:        %lu\n"
            "  Wrong Quantity:     %lu\n"
            "  Missing:            %lu\n"
            "  Extra (phantom):    %lu\n"
            "  Priority Errors:    %lu\n"
            "  Correctness Score:  %.4f\n",
            result_.total_orders, result_.total_expected,
            result_.total_actual, result_.correct_fills,
            result_.wrong_price, result_.wrong_qty,
            result_.missing_fills, result_.extra_fills,
            result_.priority_errors, result_.correctness_score());
    }

private:
    OrderBook book_;
    ExpectedFill* expected_ = nullptr;
    uint32_t expected_count_ = 0;
    ValidationResult result_{};
};

} // namespace iicpc
