// MPMC 4P4C 压测 —— 独立可执行文件，不依赖 uWS / 不启动服务
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "mpmc_queue.hpp"

int main() {
    constexpr std::uint64_t kPerProducer = 2'000'000;   // 每个生产者 200 万条
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr std::uint64_t kTotal = kPerProducer * kProducers;

    MpmcQueue<std::uint64_t, YieldWait> q(4096);

    std::atomic<std::uint64_t> pushed{0};
    std::atomic<std::uint64_t> consumed{0};
    std::atomic<bool> producers_done{false};

    const auto start = std::chrono::steady_clock::now();

    // ---- 4 个生产者：每个写 200 万条 ----
    std::vector<std::thread> producers;
    for (int i = 0; i < kProducers; ++i) {
        producers.emplace_back([&q, &pushed, i] {
            for (std::uint64_t k = 0; k < kPerProducer; ++k) {
                std::uint64_t v = (std::uint64_t)i * kPerProducer + k;
                while (!q.produce(v)) std::this_thread::yield();
                pushed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // ---- 4 个消费者：抽干直到生产者全部结束 ----
    std::vector<std::thread> consumers;
    for (int i = 0; i < kConsumers; ++i) {
        consumers.emplace_back([&q, &consumed, &producers_done] {
            std::uint64_t v;
            while (!producers_done.load(std::memory_order_relaxed)) {
                if (q.consume(v)) consumed.fetch_add(1, std::memory_order_relaxed);
                else std::this_thread::yield();
            }
            // 生产者停手后，把队列里残留抽干
            while (q.consume(v)) consumed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    // ---- 收尾：先等生产者，再放消费者走 ----
    for (auto& t : producers) t.join();
    producers_done.store(true, std::memory_order_relaxed);
    for (auto& t : consumers) t.join();

    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();

    const auto p = pushed.load();
    const auto c = consumed.load();

    std::printf("\n===== MPMC Benchmark: %dP x %dC =====\n", kProducers, kConsumers);
    std::printf("pushed    : %llu\n", (unsigned long long)p);
    std::printf("consumed  : %llu\n", (unsigned long long)c);
    std::printf("expected  : %llu\n", (unsigned long long)kTotal);
    std::printf("elapsed   : %.2f s\n", elapsed);
    std::printf("throughput: %.2f million items/sec\n", c / elapsed / 1e6);
    std::printf("result    : %s\n", (p == kTotal && c == kTotal) ? "✅ PASS" : "❌ FAIL");
    return (p == kTotal && c == kTotal) ? 0 : 1;
}
