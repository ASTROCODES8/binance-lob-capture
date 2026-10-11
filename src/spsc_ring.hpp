#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <memory>
#include <utility>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324) // Structure was padded due to alignment specifier (intentional for cacheline isolation)
#endif

namespace lob {

// Default fixed slot for live WebSocket ingestion
struct alignas(64) RingSlot {
    int64_t recv_ns{0};
    uint32_t shard{0};
    uint32_t epoch{0};
    uint64_t seq{0};
    uint32_t len{0};
    char data[65536]; // 64 KB fixed buffer per specification limits
};

/**
 * @brief High-performance Lock-Free Single-Producer Single-Consumer (SPSC) Ring Buffer.
 * 
 * Features:
 * - Cacheline isolation (alignas(64)) for head, tail, and metrics to eliminate false sharing.
 * - Power-of-2 capacity with bitmask indexing.
 * - Cached opposite indices (cached_head_ on producer, cached_tail_ on consumer) to eliminate
 *   cross-core atomic acquire loads when the queue is neither full nor empty.
 * - Zero-copy in-place slot preparation and peek operations.
 * - Overflow tracking: never silent, increments overflow counter and asserts gap flag.
 */
template <typename T, size_t Capacity = 2048>
class SpscRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
    static_assert(Capacity >= 2, "Capacity must be at least 2");

public:
    static constexpr size_t MASK = Capacity - 1;

    SpscRingBuffer()
        : slots_(std::make_unique<T[]>(Capacity)) {
    }

    ~SpscRingBuffer() = default;

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;
    SpscRingBuffer(SpscRingBuffer&&) = delete;
    SpscRingBuffer& operator=(SpscRingBuffer&&) = delete;

    // =========================================================================
    // Producer Interface (Only called from Producer / NET Thread)
    // =========================================================================

    /**
     * @brief Prepares next write slot for zero-copy in-place writing.
     * @return Pointer to slot if space available, nullptr if ring is full.
     */
    [[nodiscard]] T* prepare_write() noexcept {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        if (current_tail - cached_head_ == Capacity) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (current_tail - cached_head_ == Capacity) {
                // Ring full -> record overflow and flag gap
                overflow_count_.fetch_add(1, std::memory_order_relaxed);
                gap_flag_.store(true, std::memory_order_relaxed);
                return nullptr;
            }
        }
        return &slots_[current_tail & MASK];
    }

    /**
     * @brief Commits the previously prepared write slot, publishing it to consumer.
     */
    void commit_write() noexcept {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        tail_.store(current_tail + 1, std::memory_order_release);
    }

    /**
     * @brief Attempts to push item by copy/move into ring.
     * @return true if pushed, false if full (overflow counter incremented).
     */
    template <typename U>
    bool try_push(U&& item) noexcept {
        T* slot = prepare_write();
        if (!slot) return false;
        *slot = std::forward<U>(item);
        commit_write();
        return true;
    }

    // =========================================================================
    // Consumer Interface (Only called from Consumer / PROCESSOR Thread)
    // =========================================================================

    /**
     * @brief Peeks at the next available item without removing it (zero copy).
     * @return Pointer to item if available, nullptr if ring is empty.
     */
    [[nodiscard]] const T* peek() noexcept {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        if (current_head == cached_tail_) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (current_head == cached_tail_) {
                return nullptr;
            }
        }
        return &slots_[current_head & MASK];
    }

    /**
     * @brief Advances head pointer, consuming the front item.
     */
    void pop() noexcept {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        head_.store(current_head + 1, std::memory_order_release);
    }

    /**
     * @brief Attempts to pop front item by value into out_item.
     * @return true if popped, false if empty.
     */
    bool try_pop(T& out_item) noexcept {
        const T* item = peek();
        if (!item) return false;
        out_item = *item;
        pop();
        return true;
    }

    // =========================================================================
    // Status & Overflow Tracking (Thread-safe)
    // =========================================================================

    /**
     * @brief Checks and atomically clears the gap/overflow flag.
     */
    bool check_and_clear_gap() noexcept {
        return gap_flag_.exchange(false, std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t overflow_count() const noexcept {
        return overflow_count_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t size() const noexcept {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        return (current_tail >= current_head) ? (current_tail - current_head) : 0;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size() == 0;
    }

    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return Capacity;
    }

private:
    std::unique_ptr<T[]> slots_;

    // Producer cacheline (modified only by producer thread)
    alignas(64) std::atomic<size_t> tail_{0};
    size_t cached_head_{0};

    // Consumer cacheline (modified only by consumer thread)
    alignas(64) std::atomic<size_t> head_{0};
    size_t cached_tail_{0};

    // Metrics & Overflow cacheline
    alignas(64) std::atomic<uint64_t> overflow_count_{0};
    std::atomic<bool> gap_flag_{false};
};

} // namespace lob

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
