#ifndef OBJECT_POOL_H
#define OBJECT_POOL_H

#include <array>
#include <cassert>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace MarketMaker {

/// Fixed-size object pool with free-list allocation.
/// Pre-allocates N slots of aligned storage for type T.
/// Eliminates heap allocation on hot path for known-lifetime objects.
///
/// Not thread-safe — designed for single-threaded hot path usage.
/// Falls back to heap allocation when pool is exhausted.
template<typename T, std::size_t N>
class ObjectPool {
    static_assert(N > 0, "Pool size must be positive");

public:
    ObjectPool() noexcept {
        // Build free list: each slot stores the index of the next free slot
        for (std::size_t i = 0; i < N - 1; ++i) {
            next_free_[i] = i + 1;
        }
        next_free_[N - 1] = SENTINEL;
        free_head_ = 0;
        allocated_count_ = 0;
    }

    ~ObjectPool() {
        // All objects should be deallocated before pool destruction
        assert(allocated_count_ == 0 && "ObjectPool destroyed with live objects");
    }

    // Non-copyable, non-movable (objects hold pointers into storage)
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) = delete;
    ObjectPool& operator=(ObjectPool&&) = delete;

    /// Allocate a slot and construct T in-place with given args.
    /// Returns nullptr if pool is exhausted.
    template<typename... Args>
    [[nodiscard]] T* allocate(Args&&... args) {
        if (free_head_ == SENTINEL) {
            return nullptr; // Pool exhausted
        }

        std::size_t slot = free_head_;
        free_head_ = next_free_[slot];
        ++allocated_count_;

        // Construct object in pre-allocated storage
        T* ptr = reinterpret_cast<T*>(&storage_[slot]);
        ::new (static_cast<void*>(ptr)) T(std::forward<Args>(args)...);
        return ptr;
    }

    /// Destroy object and return slot to free list.
    void deallocate(T* ptr) noexcept {
        if (!ptr) return;

        std::size_t slot = slot_index(ptr);
        assert(slot < N && "Pointer does not belong to this pool");

        ptr->~T();
        next_free_[slot] = free_head_;
        free_head_ = slot;
        --allocated_count_;
    }

    /// Check if pointer belongs to this pool
    [[nodiscard]] bool owns(const T* ptr) const noexcept {
        if (!ptr) return false;
        auto addr = reinterpret_cast<const unsigned char*>(ptr);
        auto pool_begin = reinterpret_cast<const unsigned char*>(&storage_[0]);
        auto pool_end = reinterpret_cast<const unsigned char*>(&storage_[N]);
        return addr >= pool_begin && addr < pool_end;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return N; }
    [[nodiscard]] std::size_t allocated() const noexcept { return allocated_count_; }
    [[nodiscard]] std::size_t available() const noexcept { return N - allocated_count_; }
    [[nodiscard]] bool full() const noexcept { return free_head_ == SENTINEL; }

private:
    static constexpr std::size_t SENTINEL = static_cast<std::size_t>(-1);

    /// Compute slot index from pointer
    [[nodiscard]] std::size_t slot_index(const T* ptr) const noexcept {
        auto offset = reinterpret_cast<const unsigned char*>(ptr)
                    - reinterpret_cast<const unsigned char*>(&storage_[0]);
        return static_cast<std::size_t>(offset) / sizeof(std::aligned_storage_t<sizeof(T), alignof(T)>);
    }

    // Pre-allocated storage for N objects with proper alignment
    std::array<std::aligned_storage_t<sizeof(T), alignof(T)>, N> storage_;

    // Free list: next_free_[i] = index of next free slot after slot i
    std::array<std::size_t, N> next_free_;

    // Head of free list (SENTINEL = pool exhausted)
    std::size_t free_head_;

    // Number of currently allocated objects
    std::size_t allocated_count_;
};

} // namespace MarketMaker

#endif // OBJECT_POOL_H
