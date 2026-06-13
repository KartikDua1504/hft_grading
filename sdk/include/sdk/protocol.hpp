#pragma once

// --- Binary Wire Protocol (Exchange ↔ Strategy) ---
// Fixed-size, trivially copyable, cache-line aligned message structs.
// No serialization, no strings, no pointers — pure memcpy-safe PODs.
// All messages are 32 or 64 bytes. Native endianness (same machine).
//
// Message flow:
//   Exchange → Strategy:  MarketUpdate, OrderAck, Fill, CancelAck
//   Strategy → Exchange:  OrderEntry, CancelRequest
//   Bidirectional:        Heartbeat

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace iicpc {

// --- Constants ---
inline constexpr uint32_t PROTOCOL_VERSION  = 1;
inline constexpr uint32_t MAX_PRICE_LEVELS  = 5;   // Depth in MarketUpdate
inline constexpr uint64_t INVALID_ORDER_ID  = 0;
inline constexpr int64_t  PRICE_MULTIPLIER  = 10000; // Prices in 1/10000 units

// --- Enums (uint8_t for minimal footprint) ---
enum class Side : uint8_t {
    BUY  = 0,
    SELL = 1,
};

enum class OrderType : uint8_t {
    LIMIT  = 0,
    MARKET = 1,
    IOC    = 2,  // Immediate-or-Cancel
};

enum class MsgType : uint8_t {
    // Exchange → Strategy
    MARKET_UPDATE  = 1,
    ORDER_ACK      = 2,
    FILL           = 3,
    CANCEL_ACK     = 4,

    // Strategy → Exchange
    ORDER_ENTRY    = 10,
    CANCEL_REQUEST = 11,

    // Bidirectional
    HEARTBEAT      = 20,

    // Control
    SESSION_START  = 30,
    SESSION_END    = 31,
};

enum class AckStatus : uint8_t {
    ACCEPTED = 0,
    REJECTED_PRICE    = 1,
    REJECTED_SIZE     = 2,
    REJECTED_RISK     = 3,
    REJECTED_RATE     = 4,
    REJECTED_UNKNOWN  = 5,
    CANCELLED         = 6,
};

// --- Exchange → Strategy Messages ---

/// Top-of-book market data update (64 bytes)
struct alignas(64) MarketUpdate {
    MsgType  msg_type;         // = MARKET_UPDATE
    uint8_t  instrument_id;
    uint8_t  _pad0[2];
    uint32_t sequence;         // Exchange sequence number (gap = missed data)

    int64_t  best_bid_price;   // Best bid (PRICE_MULTIPLIER scaled)
    int64_t  best_ask_price;   // Best ask
    int32_t  best_bid_qty;
    int32_t  best_ask_qty;
    int64_t  last_trade_price;
    int32_t  last_trade_qty;
    uint64_t exchange_ts_ns;   // Exchange timestamp (nanoseconds)

    uint8_t  _pad1[4];
};
static_assert(sizeof(MarketUpdate) == 64, "MarketUpdate must be 1 cache line");
static_assert(std::is_trivially_copyable_v<MarketUpdate>);

/// Order acknowledgment (32 bytes)
struct alignas(32) OrderAck {
    MsgType   msg_type;        // = ORDER_ACK
    AckStatus status;
    uint8_t   _pad0[2];
    uint32_t  client_order_id;
    uint64_t  exchange_order_id; // 0 if rejected
    int64_t   price;
    int32_t   quantity;
    uint8_t   _pad1[4];
};
static_assert(sizeof(OrderAck) == 32);
static_assert(std::is_trivially_copyable_v<OrderAck>);

/// Fill notification (64 bytes)
struct alignas(64) Fill {
    MsgType  msg_type;         // = FILL
    Side     side;
    uint8_t  instrument_id;
    uint8_t  _pad0;
    uint32_t client_order_id;
    uint64_t exchange_order_id;
    int64_t  fill_price;
    int32_t  fill_qty;
    int32_t  remaining_qty;
    int64_t  fee;              // Transaction fee (scaled)
    int64_t  realized_pnl;
    uint64_t exchange_ts_ns;
};
static_assert(sizeof(Fill) == 64, "Fill must be 1 cache line");
static_assert(std::is_trivially_copyable_v<Fill>);

/// Cancel acknowledgment (32 bytes)
struct alignas(32) CancelAck {
    MsgType   msg_type;        // = CANCEL_ACK
    AckStatus status;
    uint8_t   _pad0[2];
    uint32_t  client_order_id;
    uint64_t  exchange_order_id;
    uint64_t  exchange_ts_ns;
};
static_assert(sizeof(CancelAck) == 32);
static_assert(std::is_trivially_copyable_v<CancelAck>);

// --- Strategy → Exchange Messages ---

/// New order entry (64 bytes)
struct alignas(64) OrderEntry {
    MsgType   msg_type;        // = ORDER_ENTRY
    Side      side;
    OrderType order_type;
    uint8_t   instrument_id;
    uint32_t  client_order_id; // Contestant-assigned (unique per session)
    int64_t   price;           // Limit price (ignored for MARKET)
    int32_t   quantity;        // Must be > 0
    uint8_t   _pad[36];
};
static_assert(sizeof(OrderEntry) == 64, "OrderEntry must be 1 cache line");
static_assert(std::is_trivially_copyable_v<OrderEntry>);

/// Cancel request (32 bytes)
struct alignas(32) CancelRequest {
    MsgType  msg_type;         // = CANCEL_REQUEST
    uint8_t  _pad0[3];
    uint32_t client_order_id;
    uint64_t exchange_order_id;
    uint8_t  _pad1[16];
};
static_assert(sizeof(CancelRequest) == 32);
static_assert(std::is_trivially_copyable_v<CancelRequest>);

// --- Bidirectional ---

/// Heartbeat (32 bytes)
struct alignas(32) Heartbeat {
    MsgType  msg_type;         // = HEARTBEAT
    uint8_t  _pad0[3];
    uint32_t sequence;
    uint64_t timestamp_ns;
    uint8_t  _pad1[16];
};
static_assert(sizeof(Heartbeat) == 32);
static_assert(std::is_trivially_copyable_v<Heartbeat>);

// --- Session Control ---

/// Session start (sent once at match begin)
struct alignas(64) SessionStart {
    MsgType  msg_type;         // = SESSION_START
    uint8_t  instrument_count;
    uint8_t  _pad0[2];
    uint32_t match_duration_ms;
    uint64_t start_timestamp_ns;
    int64_t  initial_cash;     // Starting cash balance (scaled)
    int32_t  max_position;     // Max absolute position per instrument
    int32_t  max_order_size;
    int32_t  max_orders_per_sec; // Rate limit
    uint8_t  _pad1[20];
};
static_assert(sizeof(SessionStart) == 64);
static_assert(std::is_trivially_copyable_v<SessionStart>);

/// Session end (sent once at match end)
struct alignas(64) SessionEnd {
    MsgType  msg_type;         // = SESSION_END
    uint8_t  _pad0[3];
    uint32_t total_fills;
    int64_t  final_pnl;
    int64_t  total_volume;
    int32_t  final_position;
    uint32_t total_orders;
    uint32_t total_cancels;
    uint64_t end_timestamp_ns;
    uint8_t  _pad1[16];
};
static_assert(sizeof(SessionEnd) == 64);
static_assert(std::is_trivially_copyable_v<SessionEnd>);

// --- Message Envelope Helpers ---

/// Peek at first byte to determine message type
inline MsgType peek_msg_type(const void* buf) noexcept {
    return *static_cast<const MsgType*>(buf);
}

/// Message size lookup (compile-time map)
constexpr uint32_t msg_size(MsgType type) noexcept {
    switch (type) {
        case MsgType::MARKET_UPDATE:  return sizeof(MarketUpdate);
        case MsgType::ORDER_ACK:      return sizeof(OrderAck);
        case MsgType::FILL:           return sizeof(Fill);
        case MsgType::CANCEL_ACK:     return sizeof(CancelAck);
        case MsgType::ORDER_ENTRY:    return sizeof(OrderEntry);
        case MsgType::CANCEL_REQUEST: return sizeof(CancelRequest);
        case MsgType::HEARTBEAT:      return sizeof(Heartbeat);
        case MsgType::SESSION_START:  return sizeof(SessionStart);
        case MsgType::SESSION_END:    return sizeof(SessionEnd);
        default: return 0;
    }
}

} // namespace iicpc
