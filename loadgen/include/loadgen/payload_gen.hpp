#pragma once
// =============================================================================
// payload_gen.hpp — Compile-Time Payload Template Generation
// =============================================================================
// Pre-generates all bot payloads into arena memory during initialization.
// On the hot path, we only memcpy a sequence number into a fixed offset.
// =============================================================================

#include "core/arena.hpp"
#include "core/types.hpp"
#include "loadgen/bot_fleet.hpp"
#include <cstdint>
#include <cstring>
#include <cstdio>

namespace iicpc {

/// A simple binary protocol for the dummy exchange.
/// Header: [magic(4)] [bot_id(4)] [seq_num(4)] [timestamp(8)] [payload_len(2)]
/// Total fixed header: 22 bytes + optional payload body
struct __attribute__((packed)) BenchmarkMessage {
    uint32_t magic    = 0x49494350; // "IICP"
    uint32_t bot_id   = 0;
    uint32_t seq_num  = 0;
    uint64_t tsc      = 0;
    uint16_t body_len = 0;
    // Followed by body_len bytes of body (order data, etc.)
};

static_assert(sizeof(BenchmarkMessage) == 22, "Message header must be exactly 22 bytes");

/// Ack from the exchange
struct __attribute__((packed)) AckMessage {
    uint32_t magic   = 0x41434B00; // "ACK\0"
    uint32_t bot_id  = 0;
    uint32_t seq_num = 0;
    uint64_t tsc     = 0;
};

static_assert(sizeof(AckMessage) == 20, "Ack must be exactly 20 bytes");

/// Pre-generate all payloads into arena memory.
/// Each payload is a BenchmarkMessage with a small fixed body.
inline bool generate_payloads(HugePageArena& arena, BotFleet& fleet) noexcept {
    constexpr uint16_t BODY_SIZE = 42;  // Simulated order data
    constexpr std::size_t MSG_SIZE = sizeof(BenchmarkMessage) + BODY_SIZE;

    for (std::size_t i = 0; i < fleet.count; ++i) {
        auto* buf = arena.allocate<uint8_t>(MSG_SIZE);
        if (!buf) return false;

        // Fill the template
        auto* msg = reinterpret_cast<BenchmarkMessage*>(buf);
        msg->magic    = 0x49494350;
        msg->bot_id   = fleet.bot_ids[i];
        msg->seq_num  = 0;  // Will be patched per-send
        msg->tsc      = 0;  // Will be patched per-send
        msg->body_len = BODY_SIZE;

        // Fill body with deterministic pattern
        std::memset(buf + sizeof(BenchmarkMessage), static_cast<int>(i & 0xFF), BODY_SIZE);

        fleet.payloads[i]     = buf;
        fleet.payload_lens[i] = static_cast<uint16_t>(MSG_SIZE);
    }

    std::fprintf(stderr, "[payload_gen] Pre-generated %zu payloads (%zu bytes each)\n",
                 fleet.count, MSG_SIZE);
    return true;
}

/// Hot-path: patch sequence number and TSC into an existing payload.
/// This is the ONLY mutation on the hot path — two memcpy's into known offsets.
inline void patch_payload(const uint8_t* payload, uint32_t seq_num, uint64_t tsc) noexcept {
    auto* msg = const_cast<BenchmarkMessage*>(reinterpret_cast<const BenchmarkMessage*>(payload));
    msg->seq_num = seq_num;
    msg->tsc     = tsc;
}

} // namespace iicpc
