#ifndef SPSC_RING_BUFFER_H
#define SPSC_RING_BUFFER_H

#include <atomic>
#include <array>
#include <cstddef>
#include <new>       // std::hardware_destructive_interference_size
#include <optional>
#include <type_traits>

namespace MarketMaker {

// Cache line size for padding to prevent false sharing between producer/consumer.
// Hardcoded to 64 bytes (x86_64 standard) to avoid GCC's -Winterference-size
// warning about std::hardware_destructive_interference_size being unstable.
inline constexpr size_t kCacheLineSize = 64;

/// Lock-free Single-Producer Single-Consumer ring buffer.
/// Wait-free for producer, lock-free for consumer.
/// Size must be a power of 2 for efficient index masking.
///
/// Memory ordering: acquire-release is sufficient for SPSC.
/// - Producer: store tail with release after writing data
/// - Consumer: load tail with acquire before reading data
template<typename T, size_t Size>
class SPSCRingBuffer {
    static_assert(Size > 0, "Ring buffer size must be > 0");
    static_assert((Size & (Size - 1)) == 0, "Ring buffer size must be a power of 2");
    static_assert(std::is_nothrow_move_constructible_v<T> || std::is_nothrow_copy_constructible_v<T>,
                  "T must be nothrow move or copy constructible");

public:
    SPSCRingBuffer() = default;

    // Non-copyable, non-movable (atomics can't be moved)
    SPSCRingBuffer(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;

    /// Producer: try to push an element. Returns false if buffer is full.
    /// Called from WS callback thread only.
    bool try_push(const T& item) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next_head = (head + 1) & kMask;

        // Full check: next write position would overlap read position
        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false;  // Buffer full
        }

        buffer_[head] = item;
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    /// Producer: push with move semantics
    bool try_push(T&& item) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next_head = (head + 1) & kMask;

        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false;
        }

        buffer_[head] = std::move(item);
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    /// Consumer: try to pop an element. Returns false if buffer is empty.
    /// Called from strategy thread only.
    bool try_pop(T& item) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);

        // Empty check: read position equals write position
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;  // Buffer empty
        }

        item = std::move(buffer_[tail]);
        tail_.store((tail + 1) & kMask, std::memory_order_release);
        return true;
    }

    /// Consumer: drain all items, keeping only the latest one.
    /// Useful when strategy only needs the most recent orderbook snapshot.
    /// Returns the latest item if any were available.
    std::optional<T> drain_latest() noexcept {
        std::optional<T> latest;
        T item;
        while (try_pop(item)) {
            latest.emplace(std::move(item));
        }
        return latest;
    }

    /// Check if buffer is empty (approximate - may race with producer)
    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    /// Approximate size (may race with producer/consumer)
    [[nodiscard]] size_t size_approx() const noexcept {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_acquire);
        return (head - tail) & kMask;
    }

    /// Maximum capacity
    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return Size - 1;  // One slot reserved to distinguish full from empty
    }

private:
    static constexpr size_t kMask = Size - 1;

    // Cache line aligned to prevent false sharing between producer and consumer
    alignas(kCacheLineSize) std::atomic<size_t> head_{0};  // Written by producer
    alignas(kCacheLineSize) std::atomic<size_t> tail_{0};  // Written by consumer

    // Data buffer - separate cache line from indices
    alignas(kCacheLineSize) std::array<T, Size> buffer_{};
};

} // namespace MarketMaker

#endif // SPSC_RING_BUFFER_H
