#include <cstdint>
#include <iostream>
#include <thread>

#include "ring_buffer.hpp"

// Release 模式下 NDEBUG 会禁用 assert，所以用自定义 CHECK
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << "FAILED @line " << __LINE__ << ": " #cond << "\n";  \
            return 1;                                                        \
        }                                                                    \
    } while (0)

int main() {
    // 1. 空队列 pop 必须失败
    {
        RingBuffer<int> rb(8);
        int v;
        CHECK(!rb.pop(v));
    }

    // 2. FIFO 顺序
    {
        RingBuffer<int> rb(8);
        for (int i = 0; i < 5; ++i) CHECK(rb.push(i));
        int v;
        for (int i = 0; i < 5; ++i) {
            CHECK(rb.pop(v));
            CHECK(v == i);
        }
    }

    // 3. 满队列 push 必须失败（有效容量 = size - 1）
    {
        RingBuffer<int> rb(4);
        CHECK(rb.push(1));
        CHECK(rb.push(2));
        CHECK(rb.push(3));
        CHECK(!rb.push(4));
    }

    // 4. 并发 SPSC：100 万条，不丢、不重、严格有序
    {
        RingBuffer<std::uint64_t> rb(4096);
        constexpr std::uint64_t N = 1'000'000;

        std::thread producer([&] {
            for (std::uint64_t i = 0; i < N; ++i) {
                while (!rb.push(i)) std::this_thread::yield();
            }
        });

        std::uint64_t expect = 0, got = 0;
        while (expect < N) {
            if (rb.pop(got)) {
                CHECK(got == expect);  // 任何乱序/丢数据/重复都会在这里被抓
                ++expect;
            } else {
                std::this_thread::yield();
            }
        }
        producer.join();
    }

    std::cout << "All tests passed ✅" << std::endl;
    return 0;
}
