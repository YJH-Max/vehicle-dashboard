// 独立进程 A：往共享内存写 100 条 DataPoint，验证 IPC 通道
#include "shm_ring.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

int main() {
    constexpr const char* kName     = "/vehicle_dashboard_ring";
    constexpr std::size_t kCapacity = 4096;
    constexpr int         kN        = 100;

    auto* ring = shmring::ShmRing<shmring::DataPoint>::create(kName, kCapacity);
    if (!ring) {
        std::perror("shm create");
        return 1;
    }
    std::printf("writer: created /dev/shm%s, capacity=%zu\n", kName, kCapacity);

    for (int i = 0; i < kN; ++i) {
        shmring::DataPoint d{};
        d.timestamp_ms = 1'000'000 + i;
        d.speed        = 60.0 + i * 0.1;
        d.temp         = 70.0 + i * 0.01;
        while (!ring->push(d)) std::this_thread::yield();
        std::printf("writer: #%03d pushed ts=%llu speed=%.1f\n",
                    i, (unsigned long long)d.timestamp_ms, d.speed);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::printf("writer: 100 done, wait 3s for reader to drain\n");
    std::this_thread::sleep_for(std::chrono::seconds(3));
    ring->close();
    return 0;
}
