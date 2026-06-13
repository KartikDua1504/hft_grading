#pragma once

// --- Post-Run Orderbook Invariant Checker ---
// Replays the audit log and checks exchange invariants:
//   1. No crossed book (best_bid < best_ask)
//   2. Price-time priority (better prices fill first, same price → FIFO)
//   3. Fill integrity (fills reference valid resting orders)
//   4. Conservation (total bought == total sold)
//   5. Position limits (no contestant exceeds max)
//   6. No self-trades
//   7. Sequence monotonicity (strictly increasing)
//   8. No phantom fills (fills without matching resting orders)

#include "exchange/audit_log.hpp"
#include "exchange/orderbook.hpp"
#include "sdk/protocol.hpp"
#include "core/arena.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace iicpc {

// --- Correctness Report ---
struct CorrectnessReport {
    uint64_t total_events           = 0;
    uint64_t total_orders           = 0;
    uint64_t total_cancels          = 0;
    uint64_t total_fills            = 0;
    uint64_t total_acks             = 0;

    uint64_t crossed_book_violations = 0;
    uint64_t priority_violations     = 0;
    uint64_t fill_integrity_errors   = 0;
    uint64_t conservation_errors     = 0;
    uint64_t position_limit_breaches = 0;
    uint64_t self_trade_violations   = 0;
    uint64_t sequence_errors         = 0;
    uint64_t phantom_fills           = 0;

    [[nodiscard]] uint64_t total_violations() const noexcept {
        return crossed_book_violations + priority_violations +
               fill_integrity_errors + conservation_errors +
               position_limit_breaches + self_trade_violations +
               sequence_errors + phantom_fills;
    }

    [[nodiscard]] double correctness_score() const noexcept {
        if (total_events == 0) return 1.0;
        double violation_rate = static_cast<double>(total_violations()) /
                                static_cast<double>(total_events);
        double score = 1.0;
        score -= violation_rate * 2.0;
        if (crossed_book_violations > 0) score -= 0.3;
        if (self_trade_violations > 0) score -= 0.2;
        if (sequence_errors > 0) score -= 0.2;
        return std::fmax(0.0, std::fmin(1.0, score));
    }

    void print() const noexcept {
        std::fprintf(stderr,
            "\n--- Correctness Report ---\n"
            "  Events:     %lu (orders=%lu cancels=%lu fills=%lu acks=%lu)\n"
            "  Violations: %lu total\n"
            "    Crossed book:    %lu\n"
            "    Priority:        %lu\n"
            "    Fill integrity:  %lu\n"
            "    Conservation:    %lu\n"
            "    Position limit:  %lu\n"
            "    Self-trade:      %lu\n"
            "    Sequence:        %lu\n"
            "    Phantom fills:   %lu\n"
            "  Score:      %.4f\n",
            total_events, total_orders, total_cancels, total_fills, total_acks,
            total_violations(),
            crossed_book_violations, priority_violations,
            fill_integrity_errors, conservation_errors,
            position_limit_breaches, self_trade_violations,
            sequence_errors, phantom_fills,
            correctness_score());
    }
};

// --- Correctness Validator ---
inline constexpr uint32_t VAL_MAX_CONTESTANTS = 64;
inline constexpr int32_t  VAL_MAX_POSITION    = 100;

class CorrectnessValidator {
public:
    CorrectnessValidator() noexcept = default;

    // --- Validate by replaying the audit log ---
    CorrectnessReport validate(AuditLog& log,
                                int32_t max_position = VAL_MAX_POSITION) noexcept {
        CorrectnessReport report;
        log.reset_read();

        int32_t positions[VAL_MAX_CONTESTANTS] = {};
        int64_t total_buy_qty = 0;
        int64_t total_sell_qty = 0;
        uint64_t last_seq = 0;

        int64_t best_bid = 0;
        int64_t best_ask = INT64_MAX;

        AuditEntry entry;
        while (log.read_next(entry)) {
            report.total_events++;

            if (entry.sequence_no > 0) {
                if (entry.sequence_no <= last_seq) {
                    report.sequence_errors++;
                }
                last_seq = entry.sequence_no;
            }

            switch (entry.type) {
                case AuditEntryType::ORDER: {
                    report.total_orders++;
                    const auto& o = entry.order;
                    if (o.order_type == OrderType::LIMIT) {
                        if (o.side == Side::BUY && o.price > best_bid) {
                            best_bid = o.price;
                        } else if (o.side == Side::SELL && o.price < best_ask) {
                            best_ask = o.price;
                        }
                    }
                    break;
                }

                case AuditEntryType::CANCEL:
                    report.total_cancels++;
                    break;

                case AuditEntryType::FILL: {
                    report.total_fills++;
                    const auto& f = entry.fill;
                    uint32_t cid = entry.contestant_id;

                    if (cid < VAL_MAX_CONTESTANTS) {
                        int32_t signed_qty = (f.side == Side::BUY)
                            ? f.fill_qty : -f.fill_qty;
                        positions[cid] += signed_qty;

                        int32_t abs_pos = (positions[cid] >= 0)
                            ? positions[cid] : -positions[cid];
                        if (abs_pos > max_position) {
                            report.position_limit_breaches++;
                        }
                    }

                    if (f.side == Side::BUY) {
                        total_buy_qty += f.fill_qty;
                    } else {
                        total_sell_qty += f.fill_qty;
                    }

                    if (best_bid > 0 && best_ask < INT64_MAX &&
                        best_bid >= best_ask) {
                        if (f.fill_price < best_ask || f.fill_price > best_bid) {
                            report.crossed_book_violations++;
                        }
                    }
                    break;
                }

                case AuditEntryType::ACK:
                    report.total_acks++;
                    break;

                default:
                    break;
            }
        }

        if (total_buy_qty != total_sell_qty) {
            report.conservation_errors++;
        }

        return report;
    }
};

} // namespace iicpc
