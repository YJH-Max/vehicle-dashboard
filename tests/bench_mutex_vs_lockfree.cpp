// MutexQueue vs MpmcQueue 同条件对比
// 1P1C 和 4P4C，各跑 800 万条
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "mpmc_queue.hpp"

// ---- 对照：mutex + deque ----
template <typename T>
class MutexQueue {
public:
    explicit MutexQueue(std::size_t cap) : cap_(cap) {}

    bool produce(const T& v) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (q_.size() >= cap_) return false;
        q_.push_back(v);
        return true;
    }

    bool consume(T& out) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (q_.empty()) return false;
        out = q_.front();
        q_.pop_front();
        return true;
    }

private:
    std::mutex      mtx_;
    std::deque<T>   q_;
    std::size_t     cap_;
};

// ---- 通用测试框架 ----
template <typename Queue>
void runBench(const char* name, int nProd, int nCons, std::uint64_t perProd) {
    Queue q(4096);
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> pushed{0}, consumed{0};

    auto t0 = std::chrono::steady_clock::now();

    std::vector<std::thread> ps;
    for (int i = 0; i < nProd; ++i) {
        ps.emplace_back([&, i] {
            for (std::uint64_t k = 0; k < perProd; ++k) {
                std::uint64_t v = (std::uint64_t)i * perProd + k;
                while (!q.produce(v)) std::this_thread::yield();
                pushed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::vector<std::thread> cs;
    for (int i = 0; i < nCons; ++i) {
        cs.emplace_back([&] {
            std::uint64_t v;
            while (!done.load(std::memory_order_relaxed)) {
                if (q.consume(v)) consumed.fetch_add(1, std::memory_order_relaxed);
                else std::this_thread::yield();
            }
            while (q.consume(v)) consumed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    for (auto& t : ps) t.join();
    done.store(true, std::memory_order_relaxed);
    for (auto& t : cs) t.join();

    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    auto total = consumed.load();

    std::printf("%-24s %2dP%2dC  %8.2f s  %6.2f M/s  pushed=%llu consumed=%llu %s\n",
                name, nProd, nCons, sec, total / sec / 1e6,
                (unsigned long long)pushed.load(),
                (unsigned long long)total,
                (pushed == total) ? "OK" : "DIFF");
}

int main() {
    constexpr std::uint64_t perProd = 2'000'000;

    std::printf("===== MutexQueue vs MpmcQueue =====\n");

    // 1P1C
    runBench<MutexQueue<std::uint64_t>>("Mutex   (mutex+deque)", 1, 1, perProd);
    runBench<MpmcQueue<std::uint64_t, YieldWait>>("MPMC    (Vyukov)", 1, 1, perProd);

    // 4P4C
    runBench<MutexQueue<std::uint64_t>>("Mutex   (mutex+deque)", 4, 4, perProd);
    runBench<MpmcQueue<std::uint64_t, YieldWait>>("MPMC    (Vyukov)", 4, 4, perProd);

    return 0;
}
