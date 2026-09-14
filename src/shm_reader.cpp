// 独立进程 B：从共享内存读 100 条 DataPoint，验证时间戳单调
#include "shm_ring.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

int main() {
    constexpr const char* kName = "/vehicle_dashboard_ring";
    constexpr int         kN    = 100;

    // 等 writer 创建
    shmring::ShmRing<shmring::DataPoint>* ring = nullptr;
    for (int retry = 0; retry < 50 && !ring; ++retry) {
        ring = shmring::ShmRing<shmring::DataPoint>::open(kName);
        if (!ring) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!ring) {
        std::perror("shm open");
        return 1;
    }
    std::printf("reader: opened /dev/shm%s\n", kName);

    int n = 0;
    std::uint64_t last_ts = 0;
    while (n < kN) {
        shmring::DataPoint d{};
        if (ring->pop(d)) {
            std::printf("reader: #%03d got   ts=%llu speed=%.1f temp=%.2f\n",
                        n, (unsigned long long)d.timestamp_ms, d.speed, d.temp);
            if (n > 0 && d.timestamp_ms < last_ts) {
                std::fprintf(stderr, "reader: TS regression at #%d\n", n);
                return 2;
            }
            last_ts = d.timestamp_ms;
            ++n;
        } else {
            std::this_thread::yield();
        }
    }

    std::printf("reader: read %d items, TS monotonic OK\n", n);
    ring->close();
    return 0;
}
