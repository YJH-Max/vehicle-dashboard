// 第0步占位：数据生成逻辑将在第1步（RingBuffer）实现
#include "data_generator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <thread>

void dataGenerator(DataPool& pool, int rate_hz, const std::atomic<bool>& running) {
    std::mt19937 rng{std::random_device{}()};
    std::normal_distribution<double> jitter{0.0, 1.0};

    const double dt = 1.0 / rate_hz;
    double t = 0.0;
    double speed = 45.0;
    double temp  = 55.0;

    auto next_tick = std::chrono::steady_clock::now();

    while (running.load(std::memory_order_relaxed)) {
        // 模拟"行驶工况"：正弦巡航 + 随机扰动，让曲线像真实开车
        speed += (12.0 * std::sin(t * 0.6) + jitter(rng) * 4.0) * dt;
        speed = std::clamp(speed, 0.0, 140.0);

        temp += (2.5 * std::sin(t * 0.15) + jitter(rng) * 0.8) * dt;
        temp = std::clamp(temp, 20.0, 95.0);

        const double ts = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        pool.produce({ts, speed, temp});  // 缓冲区满则丢帧（1000Hz 下不会发生）

        t += dt;
        next_tick += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(dt));
        std::this_thread::sleep_until(next_tick);  // 累计计时，避免漂移
    }
}
