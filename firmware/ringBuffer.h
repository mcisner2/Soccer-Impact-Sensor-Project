// ringBuffer.h
// Fixed-size queue for exactly one writer and one reader. That restriction is what makes it safe without locks.

#pragma once
#include <stddef.h>
#include <atomic>

template <typename T, size_t capacity>
class RingBuffer
{
    // power of two lets "index & mask" replace a slow modulo, and keeps math right when indices wrap
    static_assert(capacity > 0 && (capacity & (capacity - 1)) == 0, "capacity must be a power of two");

    public:
        // Writer only. Returns false when full: the writer can't drop the oldest item because only the reader owns tail.
        bool push(const T& item)
        {
            size_t head = headIndex.load(std::memory_order_relaxed);
            size_t tail = tailIndex.load(std::memory_order_acquire);

            if (head - tail == capacity)
            {
                return false;
            }

            slots[head & (capacity - 1)] = item;
            headIndex.store(head + 1, std::memory_order_release);  // publish only after the slot is written
            return true;
        }

        // Reader only.
        bool pop(T& item)
        {
            size_t tail = tailIndex.load(std::memory_order_relaxed);
            size_t head = headIndex.load(std::memory_order_acquire);

            if (head == tail)
            {
                return false;
            }

            item = slots[tail & (capacity - 1)];
            tailIndex.store(tail + 1, std::memory_order_release);  // slot is free only after we copied it out
            return true;
        }

        size_t count() const
        {
            return headIndex.load(std::memory_order_acquire) - tailIndex.load(std::memory_order_acquire);
        }

    private:
        T slots[capacity];
        std::atomic<size_t> headIndex { 0 };  // written by the writer only
        std::atomic<size_t> tailIndex { 0 };  // written by the reader only
};
