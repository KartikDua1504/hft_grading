#pragma once
// =============================================================================
// ring_buffer.hpp — LMAX-Style Lock-Free SPSC Ring Buffer
// =============================================================================
// This is the IPC backbone. Single-Producer Single-Consumer, zero-lock,
// zero-allocation, cache-line padded indices to prevent false sharing.
//
// Can be placed in shared memory (memfd_create) for cross-process IPC,
// or used in-process between producer/consumer threads.
//
// Memory ordering: Acquire-Release only. We NEVER use seq_cst on the hot path.
// The release on write_pos publishes the data; the acquire on write_pos
// in the consumer synchronizes-with it.
// =============================================================================

#include "core/types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <x86intrin.h>

// memfd_create may not have a glibc wrapper
#ifndef MFD_HUGETLB
#define MFD_HUGETLB 0x0004U
#endif
#ifndef MFD_HUGE_2MB
#define MFD_HUGE_2MB (21U << 26)
#endif

namespace iicpc {

/// Header placed at the start of the shared memory region.
/// Contains the padded atomic indices and metadata.
struct alignas(CACHE_LINE_SIZE) RingBufferHeader {
    // --- Producer owns this cache line ---
    PaddedAtomic<uint64_t> write_pos;

    // --- Consumer owns this cache line ---
    PaddedAtomic<uint64_t> read_pos;

    // --- Metadata (written once at init, read-only after) ---
    alignas(CACHE_LINE_SIZE) uint64_t capacity;
    uint64_t element_size;
    uint64_t magic;  // For validation: 0xDEADBEEF'CAFEBABE
    char _meta_pad[CACHE_LINE_SIZE - 3 * sizeof(uint64_t)];
};

// Compile-time verification of false sharing prevention
static_assert(
    offsetof(RingBufferHeader, read_pos) - offsetof(RingBufferHeader, write_pos) >= CACHE_LINE_SIZE,
    "CRITICAL: write_pos and read_pos MUST be on separate cache lines"
);

static constexpr uint64_t RING_BUFFER_MAGIC = 0xDEADBEEFCAFEBABEULL;

/// SPSC Ring Buffer — lock-free, bounded, cache-line padded.
///
/// Template parameters:
///   T        — Element type (must be trivially copyable)
///   Capacity — Number of elements (MUST be power of 2 for fast modulo)
///
/// Memory layout in shared region:
///   [RingBufferHeader (3 cache lines)] [T[Capacity] data array]
///
template<typename T, std::size_t Capacity>
    requires RingBufferElement<T> && (is_power_of_2(Capacity))
class SPSCRingBuffer {
public:
    static constexpr std::size_t MASK = Capacity - 1;
    static constexpr std::size_t DATA_OFFSET = align_up(sizeof(RingBufferHeader), CACHE_LINE_SIZE);
    static constexpr std::size_t TOTAL_SIZE = DATA_OFFSET + sizeof(T) * Capacity;

    /// Construct over an existing memory region (placement).
    /// The region must be at least TOTAL_SIZE bytes, cache-line aligned.
    /// @param region  Pointer to the shared memory region
    /// @param is_init If true, initialize the header (producer side). If false, attach (consumer).
    explicit SPSCRingBuffer(void* region, bool is_init) noexcept
        : header_(static_cast<RingBufferHeader*>(region))
        , data_(reinterpret_cast<T*>(static_cast<uint8_t*>(region) + DATA_OFFSET))
    {
        if (is_init) {
            // Zero the whole region first
            std::memset(region, 0, TOTAL_SIZE);
            // Initialize header
            header_->write_pos.store(0, std::memory_order_relaxed);
            header_->read_pos.store(0, std::memory_order_relaxed);
            header_->capacity = Capacity;
            header_->element_size = sizeof(T);
            header_->magic = RING_BUFFER_MAGIC;
            std::atomic_thread_fence(std::memory_order_release);
        } else {
            // Validate magic (consumer is attaching to existing buffer)
            // Spin-wait until producer has initialized
            while (header_->magic != RING_BUFFER_MAGIC) {
                _mm_pause(); // x86 spin-wait hint
            }
            std::atomic_thread_fence(std::memory_order_acquire);
        }
    }

    // Non-copyable
    SPSCRingBuffer(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;

    /// Producer: try to push an element. Returns false if buffer is full.
    /// ZERO ALLOCATION. ZERO SYSCALL.
    [[nodiscard]] bool try_push(const T& item) noexcept {
        const uint64_t w = header_->write_pos.load(std::memory_order_relaxed);
        const uint64_t r = header_->read_pos.load(std::memory_order_acquire);

        if (w - r >= Capacity) {
            return false; // Buffer full — consumer is falling behind
        }

        // Write data BEFORE publishing the write position
        data_[w & MASK] = item;

        // Release: makes the data write visible to the consumer
        header_->write_pos.store(w + 1, std::memory_order_release);
        return true;
    }

    /// Consumer: try to pop an element. Returns false if buffer is empty.
    /// ZERO ALLOCATION. ZERO SYSCALL.
    [[nodiscard]] bool try_pop(T& item) noexcept {
        const uint64_t r = header_->read_pos.load(std::memory_order_relaxed);
        const uint64_t w = header_->write_pos.load(std::memory_order_acquire);

        if (r >= w) {
            return false; // Buffer empty
        }

        // Read data BEFORE advancing the read position
        item = data_[r & MASK];

        // Release: makes the slot available for reuse by the producer
        header_->read_pos.store(r + 1, std::memory_order_release);
        return true;
    }

    /// Batch push: try to push multiple elements. Returns count actually pushed.
    std::size_t try_push_batch(const T* items, std::size_t count) noexcept {
        const uint64_t w = header_->write_pos.load(std::memory_order_relaxed);
        const uint64_t r = header_->read_pos.load(std::memory_order_acquire);
        const uint64_t available = Capacity - (w - r);
        const std::size_t to_push = count < available ? count : static_cast<std::size_t>(available);

        for (std::size_t i = 0; i < to_push; ++i) {
            data_[(w + i) & MASK] = items[i];
        }

        header_->write_pos.store(w + to_push, std::memory_order_release);
        return to_push;
    }

    /// Query how many elements are available to read
    [[nodiscard]] std::size_t size() const noexcept {
        const uint64_t w = header_->write_pos.load(std::memory_order_acquire);
        const uint64_t r = header_->read_pos.load(std::memory_order_acquire);
        return static_cast<std::size_t>(w - r);
    }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] bool full() const noexcept { return size() >= Capacity; }

    /// Verify that the indices are on separate cache lines (runtime check)
    [[nodiscard]] static bool verify_cache_line_separation(const void* region) noexcept {
        const auto* hdr = static_cast<const RingBufferHeader*>(region);
        const auto wp_addr = reinterpret_cast<std::uintptr_t>(&hdr->write_pos);
        const auto rp_addr = reinterpret_cast<std::uintptr_t>(&hdr->read_pos);
        const auto separation = (rp_addr > wp_addr) ? (rp_addr - wp_addr) : (wp_addr - rp_addr);
        return separation >= CACHE_LINE_SIZE;
    }

private:
    RingBufferHeader* header_;
    T* data_;
};

// =============================================================================
// Shared Memory Helpers (memfd_create based)
// =============================================================================

/// Create a shared memory region backed by memfd_create.
/// Returns the fd, or -1 on failure. The caller must mmap it.
inline int create_shared_ring_memory(const char* name, std::size_t size) noexcept {
    // Try with hugepages first
    int fd = static_cast<int>(::syscall(SYS_memfd_create, name, MFD_HUGETLB | MFD_HUGE_2MB));
    if (fd >= 0) {
        if (::ftruncate(fd, static_cast<off_t>(size)) == 0) {
            return fd;
        }
        ::close(fd);
    }

    // Fallback: regular memfd
    fd = static_cast<int>(::syscall(SYS_memfd_create, name, 0U));
    if (fd < 0) return -1;

    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

/// Map a memfd into the calling process's address space.
inline void* map_shared_ring_memory(int fd, std::size_t size) noexcept {
    void* ptr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    return (ptr == MAP_FAILED) ? nullptr : ptr;
}

} // namespace iicpc
