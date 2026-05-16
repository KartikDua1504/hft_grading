// =============================================================================
// arena.cpp — Huge-Page Backed Thread-Local Bump Allocator (Implementation)
// =============================================================================

#include "core/arena.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

// MAP_HUGE_2MB may not be defined on older kernels
#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB (21 << MAP_HUGE_SHIFT)
#endif

namespace iicpc {

HugePageArena::~HugePageArena() noexcept {
    if (base_) {
        ::munmap(base_, size_);
        base_ = nullptr;
    }
}

bool HugePageArena::init(std::size_t size) noexcept {
    if (base_) {
        // Already initialized — this is a programming error but we don't throw
        return hugepage_backed_;
    }

    // Round up to huge page boundary
    size = align_up(size, HUGE_PAGE_SIZE);
    
    // Attempt 1: 2MB huge pages (preferred)
    void* ptr = ::mmap(
        nullptr, size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB,
        -1, 0
    );

    if (ptr != MAP_FAILED) {
        base_ = static_cast<uint8_t*>(ptr);
        size_ = size;
        offset_ = 0;
        hugepage_backed_ = true;
        std::fprintf(stderr,
            "[arena] Allocated %zu MiB with 2MB huge pages at %p\n",
            size / (1024 * 1024), ptr);
        return true;
    }

    // Attempt 2: Regular pages with MAP_POPULATE (pre-fault to avoid page faults on hot path)
    std::fprintf(stderr,
        "[arena] Huge page mmap failed (errno=%d: %s), falling back to regular pages\n",
        errno, std::strerror(errno));

    ptr = ::mmap(
        nullptr, size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
        -1, 0
    );

    if (ptr == MAP_FAILED) {
        std::fprintf(stderr,
            "[arena] FATAL: mmap fallback also failed (errno=%d: %s)\n",
            errno, std::strerror(errno));
        return false;
    }

    // Advise kernel for huge page promotion (transparent huge pages)
    ::madvise(ptr, size, MADV_HUGEPAGE);

    base_ = static_cast<uint8_t*>(ptr);
    size_ = size;
    offset_ = 0;
    hugepage_backed_ = false;
    std::fprintf(stderr,
        "[arena] Allocated %zu MiB with regular pages (+THP advice) at %p\n",
        size / (1024 * 1024), ptr);
    return false;
}

void* HugePageArena::allocate_raw(std::size_t bytes, std::size_t alignment) noexcept {
    // Ensure minimum 64-byte alignment
    if (alignment < CACHE_LINE_SIZE) {
        alignment = CACHE_LINE_SIZE;
    }

    // Align the current offset
    const std::size_t aligned_offset = align_up(offset_, alignment);

    // Check for overflow
    if (aligned_offset + bytes > size_) {
        std::fprintf(stderr,
            "[arena] EXHAUSTED: requested %zu bytes at offset %zu / %zu\n",
            bytes, aligned_offset, size_);
        return nullptr;
    }

    void* ptr = base_ + aligned_offset;
    offset_ = aligned_offset + bytes;
    return ptr;
}

void HugePageArena::reset() noexcept {
    offset_ = 0;
    // Note: we do NOT munmap/re-mmap. The memory stays mapped.
    // This is the entire point of a bump allocator: O(1) reset.
}

HugePageArena& HugePageArena::thread_local_instance() noexcept {
    // Thread-local static: each thread gets its own arena.
    // Default size is 256 MiB per thread (configurable via init() call).
    static thread_local HugePageArena arena;
    if (!arena.is_initialized()) {
        // Per-thread arena is smaller than the main 2GB arena
        arena.init(256 * 1024 * 1024); // 256 MiB per thread
    }
    return arena;
}

} // namespace iicpc
