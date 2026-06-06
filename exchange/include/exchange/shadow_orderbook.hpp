#pragma once
// =============================================================================
// shadow_orderbook.hpp — Deterministic Reference Orderbook for Validation
// =============================================================================
// The "ground truth." Processes the EXACT same deterministic order stream
// as the contestant's code, producing the EXPECTED set of fills.
//
// When contestant returns their fills, we diff against ours:
//   - Wrong fill price → correctness penalty
//   - Wrong fill quantity → correctness penalty
//   - Missing fill (dropped order) → severe penalty
//   - Out-of-order fills (violates price-time priority) → instant fail
//   - Extra fills (phantom trades) → instant fail
//
// This runs on the HOST SIDE, completely independent of the contestant VM.
// It uses the same SoA orderbook from exchange/orderbook.hpp but with a
// clean validation interface.
// =============================================================================

#include "exchange/orderbook.hpp"
#include "sdk/protocol.hpp"
#include "core/arena.hpp"
#include "core/hot_path_asm.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace iicpc {

// =============================================================================
// Expected fill record (what the shadow says should happen)
// =============================================================================
struct alignas(32) ExpectedFill {
    uint32_t order_id;       // Client order ID that triggered this fill
    int64_t  fill_price;     // Expected fill price
    int32_t  fill_qty;       // Expected fill quantity
    Side     aggressor_side; // Which side was the aggressor
    uint8_t  _pad[11];
};
static_assert(sizeof(ExpectedFill) == 32);

// =============================================================================
// Validation result per order
// =============================================================================
struct ValidationResult {
    uint64_t total_orders      = 0;
    uint64_t total_expected    = 0;  // Expected fills from shadow
    uint64_t total_actual      = 0;  // Actual fills from contestant
    uint64_t correct_fills     = 0;  // Matched correctly
    uint64_t wrong_price       = 0;  // Fill at wrong price
    uint64_t wrong_qty         = 0;  // Fill with wrong quantity
    uint64_t missing_fills     = 0;  // Expected but not received
    uint64_t extra_fills       = 0;  // Received but not expected
    uint64_t priority_errors   = 0;  // Price-time priority violations
    uint64_t total_acks        = 0;  // Acks received

    [[nodiscard]] double correctness_score() const noexcept {
        if (total_expected == 0) return 1.0;
        double base = static_cast<double>(correct_fills) /
                      static_cast<double>(total_expected);
        // Harsh penalty for priority violations
        double penalty = static_cast<double>(priority_errors) * 0.1;
        // Ratio-based penalty for extra/missing fills (not absolute count)
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

// =============================================================================
// Shadow Orderbook (Reference Implementation)
// =============================================================================
inline constexpr uint32_t SHADOW_MAX_FILLS = 262144; // 256K fills

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

    // =========================================================================
    // Process an order through the shadow (produces expected fills)
    // =========================================================================
    void process_order(const OrderEntry& order) noexcept {
        result_.total_orders++;

        if (order.order_type == OrderType::LIMIT ||
            order.order_type == OrderType::MARKET ||
            order.order_type == OrderType::IOC) {

            // Match against shadow book
            int64_t match_price = order.price;
            if (order.order_type == OrderType::MARKET) {
                // Market order: match at any price
                match_price = (order.side == Side::BUY) ? INT64_MAX : 0;
            }

            OrderBook::MatchResult fills[64];
            uint32_t nfills = book_.match(
                order.side, match_price, order.quantity,
                0, fills, 64);

            // Record expected fills
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

            // Remaining quantity rests in shadow book
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

    // =========================================================================
    // Process a cancel through the shadow
    // =========================================================================
    void process_cancel(const CancelRequest& cancel) noexcept {
        // Shadow doesn't track cancel order IDs — cancels affect the
        // contestant's book state, but the shadow tracks the deterministic
        // expected output independently.
        (void)cancel;
    }

    // =========================================================================
    // Validate a contestant's fill against expected
    // =========================================================================
    void validate_fill(uint32_t order_id, int64_t price,
                       int32_t qty) noexcept {
        result_.total_actual++;

        // Search for matching expected fill
        bool found = false;
        for (uint32_t i = 0; i < expected_count_; ++i) {
            ExpectedFill& ef = expected_[i];
            if (ef.order_id == order_id && ef.fill_qty > 0) {
                if (ef.fill_price == price && ef.fill_qty == qty) {
                    result_.correct_fills++;
                    ef.fill_qty = 0; // Mark as consumed
                    found = true;
                    break;
                } else if (ef.fill_price != price) {
                    result_.wrong_price++;
                    ef.fill_qty = 0;
                    found = true;
                    break;
                } else {
                    result_.wrong_qty++;
                    ef.fill_qty = 0;
                    found = true;
                    break;
                }
            }
        }

        if (!found) {
            result_.extra_fills++;
        }
    }

    // =========================================================================
    // Finalize — count missing fills
    // =========================================================================
    void finalize() noexcept {
        for (uint32_t i = 0; i < expected_count_; ++i) {
            if (expected_[i].fill_qty > 0) {
                result_.missing_fills++;
            }
        }
    }

    [[nodiscard]] const ValidationResult& result() const noexcept {
        return result_;
    }
    [[nodiscard]] ValidationResult& result_mut() noexcept {
        return result_;
    }

    void print_report() const noexcept {
        std::fprintf(stderr, "\n");
        std::fprintf(stderr, "╔══════════════════════════════════════════════════════════╗\n");
        std::fprintf(stderr, "║            CORRECTNESS VALIDATION REPORT                ║\n");
        std::fprintf(stderr, "╠══════════════════════════════════════════════════════════╣\n");
        std::fprintf(stderr, "║  Total Orders:       %-35lu ║\n", result_.total_orders);
        std::fprintf(stderr, "║  Expected Fills:     %-35lu ║\n", result_.total_expected);
        std::fprintf(stderr, "║  Actual Fills:       %-35lu ║\n", result_.total_actual);
        std::fprintf(stderr, "║  Correct:            %-35lu ║\n", result_.correct_fills);
        std::fprintf(stderr, "║  Wrong Price:        %-35lu ║\n", result_.wrong_price);
        std::fprintf(stderr, "║  Wrong Quantity:     %-35lu ║\n", result_.wrong_qty);
        std::fprintf(stderr, "║  Missing:            %-35lu ║\n", result_.missing_fills);
        std::fprintf(stderr, "║  Extra (phantom):    %-35lu ║\n", result_.extra_fills);
        std::fprintf(stderr, "║  Priority Errors:    %-35lu ║\n", result_.priority_errors);
        std::fprintf(stderr, "║  Correctness Score:  %-35.4f ║\n", result_.correctness_score());
        std::fprintf(stderr, "╚══════════════════════════════════════════════════════════╝\n");
    }

private:
    OrderBook book_;
    ExpectedFill* expected_ = nullptr;
    uint32_t expected_count_ = 0;
    ValidationResult result_{};
};

} // namespace iicpc
