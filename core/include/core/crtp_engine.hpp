#pragma once
// =============================================================================
// crtp_engine.hpp — CRTP Engine Base (Static Polymorphism, Zero vtable cost)
// =============================================================================
// Replaces virtual dispatch with Curiously Recurring Template Pattern.
// The compiler resolves ALL method calls at compile-time → zero indirection,
// full inlining, perfect branch prediction.
//
// Virtual dispatch cost on Alder Lake:
//   - vtable lookup: ~5ns (L1 hit) to ~40ns (L3 if evicted)
//   - indirect branch: ~15 cycle misprediction penalty
//   - blocks inlining: compiler can't see through virtual calls
//
// CRTP cost: literally zero. The call IS the implementation after inlining.
//
// Usage:
//   class MyEngine : public EngineBase<MyEngine> {
//       // Implement required hooks:
//       void do_send_impl(BotFleet& fleet, std::size_t bot_idx) noexcept;
//       std::size_t do_recv_impl(BotFleet& fleet, TelemetryRing& ring) noexcept;
//   };
// =============================================================================

#include "core/types.hpp"
#include "core/tsc.hpp"
#include "core/compiler_hints.hpp"
#include "core/spinlock.hpp"
#include "loadgen/bot_fleet.hpp"

#include <atomic>
#include <cstdint>

namespace iicpc {

// =============================================================================
// CRTP Engine Base — All engines derive from this
// =============================================================================
template<typename Derived>
class EngineBase {
public:
    using TelemetryRing = SPSCRingBuffer<LatencySample, 1048576>;

    // No virtual destructor needed — CRTP is not used polymorphically at runtime
    EngineBase() noexcept = default;

    /// Main hot loop — sends requests for idle bots, receives responses
    IICPC_HOT IICPC_FLATTEN
    std::size_t run_batch(BotFleet& fleet, TelemetryRing& ring) noexcept {
        // Phase 1: Send for all IDLE bots (SoA scan — cache-friendly)
        send_all(fleet);

        // Phase 2: Receive responses
        return recv_all(fleet, ring);
    }

    [[nodiscard]] uint64_t total_sends() const noexcept { return total_sends_; }
    [[nodiscard]] uint64_t total_recvs() const noexcept { return total_recvs_; }

protected:
    // CRTP dispatch — resolved at compile-time, zero overhead
    Derived& self() noexcept { return static_cast<Derived&>(*this); }
    const Derived& self() const noexcept {
        return static_cast<const Derived&>(*this);
    }

    uint64_t total_sends_ = 0;
    uint64_t total_recvs_ = 0;
    uint32_t next_seq_ = 0;

private:
    IICPC_HOT
    void send_all(BotFleet& fleet) noexcept {
        for (std::size_t i = 0; i < fleet.count; ++i) {
            // Prefetch next bot's state (SoA-aware)
            if (IICPC_LIKELY(i + 8 < fleet.count)) {
                prefetch_read_l1(&fleet.states[i + 8]);
            }

            if (IICPC_UNLIKELY(fleet.states[i] != BotState::IDLE)) continue;

            // CRTP dispatch — compiler inlines the concrete implementation
            self().do_send_impl(fleet, i);
            total_sends_++;
        }
    }

    IICPC_HOT
    std::size_t recv_all(BotFleet& fleet, TelemetryRing& ring) noexcept {
        // CRTP dispatch — compiler inlines the concrete implementation
        std::size_t count = self().do_recv_impl(fleet, ring);
        total_recvs_ += count;
        return count;
    }
};

// =============================================================================
// CRTP Exchange Base — For exchange-side processing
// =============================================================================
template<typename Derived>
class ExchangeBase {
public:
    ExchangeBase() noexcept = default;

    /// Process a batch of incoming messages. Returns count processed.
    IICPC_HOT IICPC_FLATTEN
    std::size_t process_batch() noexcept {
        return self().do_process_impl();
    }

    [[nodiscard]] uint64_t total_processed() const noexcept {
        return total_processed_;
    }

protected:
    Derived& self() noexcept { return static_cast<Derived&>(*this); }
    uint64_t total_processed_ = 0;
};

// =============================================================================
// CRTP Telemetry Consumer Base
// =============================================================================
template<typename Derived>
class TelemetryBase {
public:
    TelemetryBase() noexcept = default;

    /// Process a latency sample
    IICPC_HOT IICPC_FORCE_INLINE
    void record(const LatencySample& sample) noexcept {
        self().do_record_impl(sample);
    }

    /// Publish metrics snapshot
    void publish() noexcept {
        self().do_publish_impl();
    }

protected:
    Derived& self() noexcept { return static_cast<Derived&>(*this); }
};

} // namespace iicpc
