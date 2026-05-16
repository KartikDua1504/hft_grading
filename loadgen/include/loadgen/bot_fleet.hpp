#pragma once
// =============================================================================
// bot_fleet.hpp — SoA Bot Fleet State for Data Plane
// =============================================================================
// Strict Struct-of-Arrays layout. Each field is a contiguous, cache-line
// aligned array. This maximizes L1/L2 cache hits when iterating over a
// single field (e.g., all send timestamps) and enables SIMD auto-vectorization.
// =============================================================================

#include "core/types.hpp"
#include "core/arena.hpp"
#include <cstdint>
#include <cstring>

namespace iicpc {

/// Bot state machine
enum class BotState : uint8_t {
    IDLE     = 0,
    SENDING  = 1,
    WAITING  = 2,
    RECEIVED = 3,
};

/// SoA bot fleet — all arrays allocated from the arena.
/// No heap allocations. No std::vector.
struct BotFleet {
    std::size_t count = 0;

    // === SoA arrays — each on its own cache-line aligned base ===
    uint32_t*       bot_ids       = nullptr;
    uint64_t*       sequence_nums = nullptr;
    BotState*       states        = nullptr;
    uint64_t*       send_tsc      = nullptr;  // TSC at send
    uint64_t*       recv_tsc      = nullptr;  // TSC at receive
    int32_t*        socket_fds    = nullptr;
    const uint8_t** payloads      = nullptr;  // Pointers into pre-gen'd payload arena
    uint16_t*       payload_lens  = nullptr;

    /// Initialize the fleet: allocate all SoA arrays from the arena.
    /// MUST be called before use.
    bool init(HugePageArena& arena, std::size_t num_bots) noexcept {
        count = num_bots;
        bot_ids       = arena.allocate<uint32_t>(count);
        sequence_nums = arena.allocate<uint64_t>(count);
        states        = arena.allocate<BotState>(count);
        send_tsc      = arena.allocate<uint64_t>(count);
        recv_tsc      = arena.allocate<uint64_t>(count);
        socket_fds    = arena.allocate<int32_t>(count);
        payloads      = arena.allocate<const uint8_t*>(count);
        payload_lens  = arena.allocate<uint16_t>(count);

        if (!bot_ids || !sequence_nums || !states || !send_tsc ||
            !recv_tsc || !socket_fds || !payloads || !payload_lens) {
            return false;
        }

        // Zero-initialize all arrays
        std::memset(bot_ids, 0, sizeof(uint32_t) * count);
        std::memset(sequence_nums, 0, sizeof(uint64_t) * count);
        std::memset(states, 0, sizeof(BotState) * count);
        std::memset(send_tsc, 0, sizeof(uint64_t) * count);
        std::memset(recv_tsc, 0, sizeof(uint64_t) * count);
        std::memset(socket_fds, 0xFF, sizeof(int32_t) * count); // -1 = no socket
        std::memset(payloads, 0, sizeof(const uint8_t*) * count);
        std::memset(payload_lens, 0, sizeof(uint16_t) * count);

        // Assign bot IDs
        for (std::size_t i = 0; i < count; ++i) {
            bot_ids[i] = static_cast<uint32_t>(i);
        }

        return true;
    }
};

} // namespace iicpc
