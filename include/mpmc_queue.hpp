#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <thread>

#ifndef CACHELINE_SIZE
#define CACHELINE_SIZE 64
#endif

struct BusySpinWait {
    static void wait() noexcept {}
};
struct YieldWait {
    static void wait() noexcept { std::this_thread::yield(); }
};

template <typename T, typename WaitStrategy = YieldWait>
class MpmcQueue {
public:
    explicit MpmcQueue(std::size_t capacity)
        : capacity_(round_up_pow2(capacity)),
          mask_(capacity_ - 1),
          cells_(capacity_) {
        for (std::size_t i = 0; i < capacity_; ++i)
            cells_[i].seq.store(i, std::memory_order_relaxed);
    }

    bool produce(const T& v) {
        for (;;) {
            std::size_t pos = tail_.load(std::memory_order_relaxed);
            Cell& c = cells_[pos & mask_];
            std::size_t seq = c.seq.load(std::memory_order_acquire);
            std::intptr_t diff = (std::intptr_t)seq - (std::intptr_t)pos;

            if (diff == 0) {
                if (tail_.compare_exchange_weak(
                        pos, pos + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    c.data = v;
                    c.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;
            } else {
                WaitStrategy::wait();
            }
        }
    }

    bool consume(T& out) {
        for (;;) {
            std::size_t pos = head_.load(std::memory_order_relaxed);
            Cell& c = cells_[pos & mask_];
            std::size_t seq = c.seq.load(std::memory_order_acquire);
            std::intptr_t diff = (std::intptr_t)seq - (std::intptr_t)(pos + 1);

            if (diff == 0) {
                if (head_.compare_exchange_weak(
                        pos, pos + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    out = c.data;
                    c.seq.store(pos + capacity_, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;
            } else {
                WaitStrategy::wait();
            }
        }
    }

    std::size_t capacity() const noexcept { return capacity_; }

private:
    struct Cell {
        std::atomic<std::size_t> seq{0};
        T data{};
    };

    alignas(CACHELINE_SIZE) std::atomic<std::size_t> head_{0};
    alignas(CACHELINE_SIZE) std::atomic<std::size_t> tail_{0};

    const std::size_t capacity_;
    const std::size_t mask_;
    std::vector<Cell> cells_;

    static std::size_t round_up_pow2(std::size_t n) {
        std::size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }
};
