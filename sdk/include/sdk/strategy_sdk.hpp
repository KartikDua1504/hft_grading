#pragma once

// --- Contestant Orderbook Engine Interface ---
// Contestants implement IStrategy to build their matching engine.
// Receives OrderEntry/CancelRequest, returns ContestantResponse.
// on_order() and on_cancel() must return quickly — blocking lowers throughput.

#include "sdk/protocol.hpp"

#include <cstdint>

namespace iicpc {

// --- Contestant Response ---
// Sent back to the platform after processing an order.
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

// --- Response Sender ---
// For aggressive orders matching multiple levels, call send() per fill.
class IResponseSender {
public:
    virtual ~IResponseSender() = default;
    virtual bool send(const ContestantResponse& resp) noexcept = 0;
};

// --- Strategy Interface (contestants implement this) ---
class IStrategy {
public:
    virtual ~IStrategy() = default;

    /// Called once at session start. Initialize your orderbook here.
    virtual void on_session_start(const SessionStart& session) noexcept {}

    /// Called for every incoming order. Process through your matching engine.
    /// Use 'sender' to send ContestantResponse(s):
    ///   - Resting order: send one ACK
    ///   - Aggressive fill: send FILL(s) then ACK
    virtual void on_order(const OrderEntry& order,
                          IResponseSender& sender) noexcept = 0;

    /// Called for every cancel request. Lookup and cancel the order.
    virtual void on_cancel(const CancelRequest& cancel,
                           IResponseSender& sender) noexcept = 0;

    /// Called once at session end.
    virtual void on_session_end(const SessionEnd& session) noexcept {}
};

// --- Strategy Factory ---
// Contestants implement this to instantiate their engine:
//   iicpc::IStrategy* iicpc::create_strategy() {
//       static MyOrderbook instance;
//       return &instance;
//   }
extern IStrategy* create_strategy();

} // namespace iicpc
