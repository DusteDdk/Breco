#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>

namespace breco {

template <typename T, std::size_t Capacity>
class SpscQueue {
public:
    static_assert(Capacity > 1, "Capacity must be greater than one");

    bool tryPush(const T& item) {
        const std::size_t head = m_head.load(std::memory_order_relaxed);
        const std::size_t next = increment(head);
        if (next == m_tail.load(std::memory_order_acquire)) {
            return false;
        }
        m_buffer[head] = item;
        m_head.store(next, std::memory_order_release);
        return true;
    }

    bool tryPop(T& out) {
        const std::size_t tail = m_tail.load(std::memory_order_relaxed);
        if (tail == m_head.load(std::memory_order_acquire)) {
            return false;
        }
        out = *m_buffer[tail];
        m_buffer[tail].reset();
        m_tail.store(increment(tail), std::memory_order_release);
        return true;
    }

private:
    static constexpr std::size_t increment(std::size_t value) { return (value + 1) % Capacity; }

    std::array<std::optional<T>, Capacity> m_buffer{};
    std::atomic<std::size_t> m_head{0};
    std::atomic<std::size_t> m_tail{0};
};

}  // namespace breco
