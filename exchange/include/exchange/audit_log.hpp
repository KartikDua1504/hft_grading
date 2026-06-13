#pragma once

// --- Append-Only Binary Audit Log ---
// Every sequenced event is written to an mmap'd file.
// Enables deterministic replay, post-hoc validation, and forensics.
// Fixed-size AuditEntry records, zero-copy via mmap.

#include "exchange/sequencer.hpp"
#include "sdk/protocol.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace iicpc {

// --- Audit Entry Types ---
enum class AuditEntryType : uint8_t {
    ORDER       = 1,
    CANCEL      = 2,
    FILL        = 3,
    ACK         = 4,
    CANCEL_ACK  = 5,
    MARKET_DATA = 6,
    SESSION     = 7,
};

// --- Audit Entry (128 bytes, cache-line aligned) ---
struct alignas(64) AuditEntry {
    // Header (16 bytes)
    AuditEntryType type;
    uint8_t        _pad0[3];
    uint32_t       contestant_id;
    uint64_t       sequence_no;

    // Timestamp (8 bytes)
    uint64_t       tsc;             // rdtsc at log time

    // Payload — fits any message type
    union {
        OrderEntry    order;        // 64 bytes
        CancelRequest cancel;       // 32 bytes
        Fill          fill;         // 64 bytes
        OrderAck      ack;          // 32 bytes
        CancelAck     cancel_ack;   // 32 bytes
        MarketUpdate  market_data;  // 64 bytes
        uint8_t       raw[96];
    };
};
static_assert(std::is_trivially_copyable_v<AuditEntry>);

// --- Audit Log (mmap'd append-only file) ---
inline constexpr uint64_t AUDIT_LOG_MAX_ENTRIES = 1 << 20; // 1M entries
inline constexpr uint64_t AUDIT_LOG_MAX_SIZE =
    AUDIT_LOG_MAX_ENTRIES * sizeof(AuditEntry);

class AuditLog {
public:
    AuditLog() noexcept = default;
    ~AuditLog() noexcept { close(); }

    AuditLog(const AuditLog&) = delete;
    AuditLog& operator=(const AuditLog&) = delete;

    // --- Open for writing (creates/truncates, mmaps) ---
    bool open_write(const char* path) noexcept {
        fd_ = ::open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd_ < 0) {
            std::fprintf(stderr, "[audit] Failed to create %s: %s\n",
                         path, strerror(errno));
            return false;
        }

        if (::ftruncate(fd_, static_cast<off_t>(AUDIT_LOG_MAX_SIZE)) < 0) {
            std::fprintf(stderr, "[audit] ftruncate failed: %s\n",
                         strerror(errno));
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        base_ = static_cast<AuditEntry*>(
            ::mmap(nullptr, AUDIT_LOG_MAX_SIZE,
                   PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
        if (base_ == MAP_FAILED) {
            std::fprintf(stderr, "[audit] mmap failed: %s\n",
                         strerror(errno));
            base_ = nullptr;
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        ::madvise(base_, AUDIT_LOG_MAX_SIZE, MADV_SEQUENTIAL);

        write_pos_ = 0;
        readonly_ = false;
        std::fprintf(stderr, "[audit] Opened for writing: %s (%lu MB)\n",
                     path, AUDIT_LOG_MAX_SIZE / (1024 * 1024));
        return true;
    }

    // --- Open for reading (readonly mmap) ---
    bool open_read(const char* path) noexcept {
        fd_ = ::open(path, O_RDONLY);
        if (fd_ < 0) return false;

        struct stat st;
        if (::fstat(fd_, &st) < 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        file_size_ = static_cast<uint64_t>(st.st_size);
        base_ = static_cast<AuditEntry*>(
            ::mmap(nullptr, file_size_, PROT_READ, MAP_PRIVATE, fd_, 0));
        if (base_ == MAP_FAILED) {
            base_ = nullptr;
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        ::madvise(base_, file_size_, MADV_SEQUENTIAL);
        max_entries_ = file_size_ / sizeof(AuditEntry);
        read_pos_ = 0;
        readonly_ = true;
        return true;
    }

    // --- Log an order ---
    void log_order(uint64_t seq_no, uint32_t contestant_id,
                   const OrderEntry& order, uint64_t tsc) noexcept {
        if (!can_write()) return;
        AuditEntry& e = base_[write_pos_++];
        e.type = AuditEntryType::ORDER;
        e.contestant_id = contestant_id;
        e.sequence_no = seq_no;
        e.tsc = tsc;
        std::memcpy(&e.order, &order, sizeof(OrderEntry));
    }

    void log_cancel(uint64_t seq_no, uint32_t contestant_id,
                    const CancelRequest& cancel, uint64_t tsc) noexcept {
        if (!can_write()) return;
        AuditEntry& e = base_[write_pos_++];
        e.type = AuditEntryType::CANCEL;
        e.contestant_id = contestant_id;
        e.sequence_no = seq_no;
        e.tsc = tsc;
        std::memcpy(&e.cancel, &cancel, sizeof(CancelRequest));
    }

    void log_fill(uint64_t seq_no, uint32_t contestant_id,
                  const Fill& fill, uint64_t tsc) noexcept {
        if (!can_write()) return;
        AuditEntry& e = base_[write_pos_++];
        e.type = AuditEntryType::FILL;
        e.contestant_id = contestant_id;
        e.sequence_no = seq_no;
        e.tsc = tsc;
        std::memcpy(&e.fill, &fill, sizeof(Fill));
    }

    void log_ack(uint64_t seq_no, uint32_t contestant_id,
                 const OrderAck& ack, uint64_t tsc) noexcept {
        if (!can_write()) return;
        AuditEntry& e = base_[write_pos_++];
        e.type = AuditEntryType::ACK;
        e.contestant_id = contestant_id;
        e.sequence_no = seq_no;
        e.tsc = tsc;
        std::memcpy(&e.ack, &ack, sizeof(OrderAck));
    }

    // --- Read (for replay / validation) ---
    bool read_next(AuditEntry& out) noexcept {
        if (!readonly_ || read_pos_ >= max_entries_) return false;
        const AuditEntry& e = base_[read_pos_];
        if (e.type == static_cast<AuditEntryType>(0)) return false;
        std::memcpy(&out, &e, sizeof(AuditEntry));
        read_pos_++;
        return true;
    }

    void reset_read() noexcept { read_pos_ = 0; }

    // --- Flush + close ---
    void flush() noexcept {
        if (base_ && !readonly_) {
            ::msync(base_, write_pos_ * sizeof(AuditEntry), MS_SYNC);
        }
    }

    void close() noexcept {
        if (base_) {
            uint64_t size = readonly_ ? file_size_ : AUDIT_LOG_MAX_SIZE;
            ::munmap(base_, size);
            base_ = nullptr;
        }
        if (fd_ >= 0) {
            if (!readonly_ && write_pos_ > 0) {
                ::ftruncate(fd_, static_cast<off_t>(
                    write_pos_ * sizeof(AuditEntry)));
            }
            ::close(fd_);
            fd_ = -1;
        }
    }

    [[nodiscard]] uint64_t entry_count() const noexcept { return write_pos_; }
    [[nodiscard]] bool is_open() const noexcept { return base_ != nullptr; }

    void print_stats() const noexcept {
        std::fprintf(stderr, "[audit] Entries: %lu (%.2f MB)\n",
                     write_pos_,
                     static_cast<double>(write_pos_ * sizeof(AuditEntry)) / (1024.0 * 1024.0));
    }

private:
    [[nodiscard]] bool can_write() const noexcept {
        return base_ && !readonly_ && write_pos_ < AUDIT_LOG_MAX_ENTRIES;
    }

    AuditEntry* base_ = nullptr;
    int         fd_ = -1;
    uint64_t    write_pos_ = 0;
    uint64_t    read_pos_ = 0;
    uint64_t    max_entries_ = 0;
    uint64_t    file_size_ = 0;
    bool        readonly_ = false;
};

} // namespace iicpc
