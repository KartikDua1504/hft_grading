#pragma once
// =============================================================================
// strategy_sdk.hpp — Contestant Strategy Interface
// =============================================================================
// Contestants implement IStrategy. The platform calls on_market_data() on
// every tick. Contestants submit orders via the Gateway reference passed
// at init. PnL is tracked by the exchange — contestants see it in Fill msgs.
//
// Rules:
//   - on_market_data() must return quickly (<100µs ideally)
//   - Blocking = missing ticks = losing money
//   - No heap allocation in the hot loop (use stack/arena)
//   - Single instrument (instrument_id = 0)
// =============================================================================

#include "sdk/protocol.hpp"

namespace iicpc {

// =============================================================================
// Order submission interface (provided by the platform)
// =============================================================================
class IOrderGateway {
public:
    virtual ~IOrderGateway() = default;

    /// Submit a new order. Returns true if accepted for sending.
    /// Does NOT mean the order was matched — wait for OrderAck/Fill.
    [[nodiscard]] virtual bool send_order(const OrderEntry& order) noexcept = 0;

    /// Submit a cancel request. Returns true if accepted for sending.
    [[nodiscard]] virtual bool send_cancel(const CancelRequest& cancel) noexcept = 0;

    /// Get current contestant position (updated after each fill)
    [[nodiscard]] virtual int32_t position() const noexcept = 0;

    /// Get current unrealized PnL (mark-to-market)
    [[nodiscard]] virtual int64_t unrealized_pnl() const noexcept = 0;

    /// Get current realized PnL (from closed trades)
    [[nodiscard]] virtual int64_t realized_pnl() const noexcept = 0;

    /// Get total PnL (realized + unrealized)
    [[nodiscard]] virtual int64_t total_pnl() const noexcept = 0;

    /// Generate a unique client order ID
    [[nodiscard]] virtual uint32_t next_order_id() noexcept = 0;
};

// =============================================================================
// Strategy interface (contestants implement this)
// =============================================================================
class IStrategy {
public:
    virtual ~IStrategy() = default;

    /// Called once at session start. Store the gateway reference.
    /// Use this to initialize your data structures.
    virtual void on_session_start(IOrderGateway& gateway,
                                   const SessionStart& session) noexcept = 0;

    /// Called on every market data tick. This is the HOT PATH.
    /// Read the update, decide whether to trade, submit orders via gateway.
    /// MUST return quickly — if you block, you miss ticks and lose money.
    virtual void on_market_data(const MarketUpdate& update) noexcept = 0;

    /// Called when your order is acknowledged (accepted/rejected).
    virtual void on_order_ack(const OrderAck& ack) noexcept = 0;

    /// Called when your order is filled (partially or fully).
    /// PnL fields are updated by the platform.
    virtual void on_fill(const Fill& fill) noexcept = 0;

    /// Called when your cancel request is acknowledged.
    virtual void on_cancel_ack(const CancelAck& ack) noexcept = 0;

    /// Called once at session end. Print stats, cleanup, etc.
    virtual void on_session_end(const SessionEnd& session) noexcept = 0;
};

// =============================================================================
// Strategy factory — contestants must implement this function
// =============================================================================
// The platform calls create_strategy() to instantiate the contestant's strategy.
// Contestants define this in their source file:
//
//   iicpc::IStrategy* iicpc::create_strategy() {
//       static MyStrategy instance;
//       return &instance;
//   }
//
// Using static instance avoids heap allocation. The strategy lives for the
// entire session duration.
// =============================================================================
extern IStrategy* create_strategy();

} // namespace iicpc
