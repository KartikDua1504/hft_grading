#pragma once
// =============================================================================
// flat_map.hpp — Robin Hood Open-Addressing Hash Map (Header-Only)
// =============================================================================
// Fixed-capacity, zero-allocation hash map for O(1) lookups on the hot path.
// Uses Robin Hood hashing to keep probe sequences short.
// All memory comes from the arena allocator.
// =============================================================================

#include "core/types.hpp"
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <optional>

namespace iicpc {

// FNV-1a hash — fast, good distribution, constexpr-friendly
constexpr uint64_t fnv1a_hash(const void* data, std::size_t len) noexcept {
    constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
    constexpr uint64_t FNV_PRIME  = 1099511628211ULL;
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t hash = FNV_OFFSET;
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= FNV_PRIME;
    }
    return hash;
}

constexpr uint64_t fnv1a_hash_u64(uint64_t key) noexcept {
    return fnv1a_hash(&key, sizeof(key));
}

template<typename Key, typename Value, std::size_t Capacity>
    requires (is_power_of_2(Capacity))
          && std::is_trivially_copyable_v<Key>
          && std::is_trivially_copyable_v<Value>
class FlatMap {
    static constexpr std::size_t MASK = Capacity - 1;
    static constexpr uint8_t EMPTY_PSL = 0;

    struct Slot {
        Key key;
        Value value;
        uint8_t psl;  // Probe Sequence Length (0 = empty)
    };

public:
    FlatMap() noexcept { clear(); }

    void clear() noexcept {
        for (std::size_t i = 0; i < Capacity; ++i) {
            slots_[i].psl = EMPTY_PSL;
        }
        size_ = 0;
    }

    /// Insert or update. Returns true if inserted, false if updated.
    bool insert(const Key& key, const Value& value) noexcept {
        uint64_t hash = fnv1a_hash(&key, sizeof(Key));
        std::size_t idx = hash & MASK;
        uint8_t psl = 1;
        Slot incoming{key, value, psl};

        while (true) {
            if (slots_[idx].psl == EMPTY_PSL) {
                slots_[idx] = incoming;
                size_++;
                return true;
            }
            if (std::memcmp(&slots_[idx].key, &key, sizeof(Key)) == 0) {
                slots_[idx].value = value;
                return false;
            }
            // Robin Hood: steal from the rich
            if (incoming.psl > slots_[idx].psl) {
                Slot tmp = slots_[idx];
                slots_[idx] = incoming;
                incoming = tmp;
            }
            incoming.psl++;
            idx = (idx + 1) & MASK;
        }
    }

    /// Lookup. Returns pointer to value or nullptr.
    [[nodiscard]] Value* find(const Key& key) noexcept {
        uint64_t hash = fnv1a_hash(&key, sizeof(Key));
        std::size_t idx = hash & MASK;
        uint8_t psl = 1;

        while (true) {
            if (slots_[idx].psl == EMPTY_PSL || psl > slots_[idx].psl) {
                return nullptr;
            }
            if (std::memcmp(&slots_[idx].key, &key, sizeof(Key)) == 0) {
                return &slots_[idx].value;
            }
            psl++;
            idx = (idx + 1) & MASK;
        }
    }

    [[nodiscard]] const Value* find(const Key& key) const noexcept {
        return const_cast<FlatMap*>(this)->find(key);
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] double load_factor() const noexcept {
        return static_cast<double>(size_) / Capacity;
    }

private:
    Slot slots_[Capacity];
    std::size_t size_ = 0;
};

} // namespace iicpc
