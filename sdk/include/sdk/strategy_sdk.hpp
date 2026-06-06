#pragma once
// =============================================================================
// strategy_sdk.hpp — Contestant Orderbook Engine Interface
// =============================================================================
// Contestants implement IStrategy to build their own matching engine.
// The platform blasts orders at your engine and measures:
//   - Correctness: do your fills match the reference shadow orderbook?
//   - Throughput:  how many orders/sec can you process?
//   - Latency:     how fast is your order-to-response time?
//
// You receive OrderEntry and CancelRequest messages, and must fill
// ContestantResponse structs with your matching results.
//
// Rules:
//   - on_order() and on_cancel() must return quickly (<1µs ideally)
//   - Blocking = low throughput = low score
//   - No heap allocation in the hot loop (use stack/arena)
//   - Single instrument (instrument_id = 0)
// =============================================================================

#include "sdk/protocol.hpp"

#include <cstdint>

namespace iicpc {

// =============================================================================
// Contestant's response (what you send back after processing an order)
// =============================================================================
// The platform reads these to measure your correctness and latency.
// You fill order_id, response_type, ack_status, fill fields, etc.
// The recv_tsc/send_tsc fields are filled by the platform (ignore them).
// =============================================================================
struct alignas(64) ContestantResponse {
    uint32_t order_id;         // Echo back the order's client_order_id
    MsgType  response_type;    // ORDER_ACK or FILL
    AckStatus ack_status;      // If ORDER_ACK: ACCEPTED, REJECTED_*, CANCELLED
    uint8_t  _pad0;
    uint8_t  num_fills;        // Number of fills this order generated
    int64_t  fill_price;       // Fill price (if response_type == FILL)
    int32_t  fill_qty;         // Fill quantity (if response_type == FILL)
    int32_t  remaining_qty;    // Remaining on resting order
    uint64_t recv_tsc;         // (platform use — do not set)
    uint64_t send_tsc;         // (platform use — do not set)
    uint8_t  _pad1[16];
};
static_assert(sizeof(ContestantResponse) == 64);

// =============================================================================
// Response sender — allows sending multiple responses per order
// =============================================================================
// For aggressive orders that match multiple resting levels, call
// sender.send() once per fill, then once for the ack.
// =============================================================================
class IResponseSender {
public:
    virtual ~IResponseSender() = default;
    virtual bool send(const ContestantResponse& resp) noexcept = 0;
};

// =============================================================================
// Strategy interface (contestants implement this)
// =============================================================================
class IStrategy {
public:
    virtual ~IStrategy() = default;

    /// Called once at session start. Initialize your orderbook here.
    virtual void on_session_start(const SessionStart& session) noexcept {}

    /// Called for every incoming order. THIS IS THE HOT PATH.
    /// Process the order through your matching engine.
    /// Use 'sender' to send one or more ContestantResponse:
    ///   - For a resting order: send one ACK
    ///   - For an aggressive order that fills: send FILL(s) then ACK
    /// MUST return quickly — blocking = low throughput = low score.
    virtual void on_order(const OrderEntry& order,
                          IResponseSender& sender) noexcept = 0;

    /// Called for every cancel request.
    /// Look up the order and cancel it. Send a CANCEL_ACK response.
    virtual void on_cancel(const CancelRequest& cancel,
                           IResponseSender& sender) noexcept = 0;

    /// Called once at session end. Print stats, cleanup, etc.
    virtual void on_session_end(const SessionEnd& session) noexcept {}
};

// =============================================================================
// Strategy factory — contestants must implement this function
// =============================================================================
// The platform calls create_strategy() to instantiate the contestant's engine.
// Contestants define this in their source file:
//
//   iicpc::IStrategy* iicpc::create_strategy() {
//       static MyOrderbook instance;
//       return &instance;
//   }
//
// Using static instance avoids heap allocation. The engine lives for the
// entire session duration.
// =============================================================================
extern IStrategy* create_strategy();

} // namespace iicpc
