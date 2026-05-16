#pragma once
// =============================================================================
// arena.hpp — Huge-Page Backed Thread-Local Bump Allocator
// =============================================================================
// This is the memory foundation of the entire system. ALL hot-path allocations
// go through this allocator. No new, no delete, no malloc on the hot path.
//
// Design decisions:
//   1. mmap with MAP_HUGETLB|MAP_HUGE_2MB to eliminate TLB thrashing
//   2. Thread-local instances — zero contention, zero atomics
//   3. 64-byte aligned returns — every allocation lands on a cache line boundary
//   4. Bump-only semantics — no free(), only reset() between batches
//   5. Fallback to regular mmap if hugepages unavailable (graceful degradation)
// =============================================================================

#include "core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <type_traits>

namespace iicpc {

class HugePageArena {
public:
    /// Construct an arena. Does NOT allocate — call init() explicitly.
    HugePageArena() noexcept = default;

    /// Non-copyable, non-movable (owns raw mmap'd memory)
    HugePageArena(const HugePageArena&) = delete;
    HugePageArena& operator=(const HugePageArena&) = delete;
    HugePageArena(HugePageArena&&) = delete;
    HugePageArena& operator=(HugePageArena&&) = delete;

    ~HugePageArena() noexcept;

    /// Initialize the arena with the given size.
    /// @param size Total arena size in bytes (will be rounded up to huge page boundary)
    /// @return true if hugepages were used, false if fell back to regular pages
    [[nodiscard]] bool init(std::size_t size = DEFAULT_ARENA_SIZE) noexcept;

    /// Allocate count objects of type T, aligned to 64-byte boundary.
    /// Returns nullptr if arena is exhausted (should never happen if sized correctly).
    template<typename T>
    [[nodiscard]] T* allocate(std::size_t count = 1) noexcept {
        static_assert(std::is_trivially_constructible_v<T>,
            "Arena only supports trivially constructible types");

        const std::size_t bytes = sizeof(T) * count;
        void* ptr = allocate_raw(bytes, alignof(T) < CACHE_LINE_SIZE ? CACHE_LINE_SIZE : alignof(T));
        return static_cast<T*>(ptr);
    }

    /// Allocate raw bytes with specified alignment.
    [[nodiscard]] void* allocate_raw(std::size_t bytes, std::size_t alignment = CACHE_LINE_SIZE) noexcept;

    /// Allocate and return a span (for SoA arrays).
    template<typename T>
    [[nodiscard]] std::span<T> allocate_span(std::size_t count) noexcept {
        T* ptr = allocate<T>(count);
        if (!ptr) return {};
        return std::span<T>(ptr, count);
    }

    /// Reset the arena — all previous allocations become invalid.
    /// This is the "deallocation" mechanism: O(1), just resets the bump pointer.
    void reset() noexcept;

    /// Query arena state
    [[nodiscard]] std::size_t capacity() const noexcept { return size_; }
    [[nodiscard]] std::size_t used() const noexcept { return offset_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return size_ - offset_; }
    [[nodiscard]] bool is_hugepage_backed() const noexcept { return hugepage_backed_; }
    [[nodiscard]] bool is_initialized() const noexcept { return base_ != nullptr; }

    /// Get the thread-local arena instance. Lazily initialized on first call.
    static HugePageArena& thread_local_instance() noexcept;

private:
    uint8_t* base_ = nullptr;          // Base of mmap'd region
    std::size_t size_ = 0;             // Total arena size
    std::size_t offset_ = 0;           // Current bump pointer offset
    bool hugepage_backed_ = false;     // Whether we got actual hugepages
};

} // namespace iicpc
