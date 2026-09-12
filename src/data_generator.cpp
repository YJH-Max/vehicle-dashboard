#include "data_generator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <random>
#include <thread>

void dataGenerator(DataPool& pool,
                   TopicBus<DataPoint>& bus,
                   int rate_hz,
                   const std::atomic<bool>& running) {
    std::mt19937 rng{std::random_device{}()};
    std::normal_distribution<double> jitter{0.0, 1.0};

    const double dt = 1.0 / rate_hz;
    double t = 0.0;
    double speed = 45.0;
    double temp  = 55.0;

    auto next_tick = std::chrono::steady_clock::now();

    while (running.load(std::memory_order_relaxed)) {
        speed += (12.0 * std::sin(t * 0.6) + jitter(rng) * 4.0) * dt;
        speed = std::clamp(speed, 0.0, 140.0);

        temp += (2.5 * std::sin(t * 0.15) + jitter(rng) * 0.8) * dt;
        temp = std::clamp(temp, 20.0, 95.0);

        DataPoint d{Timestamp::now(), speed, temp};

        // 尽力而为：pool 满了不阻塞主实时链路
        pool.produce(d);

        // 实时总线：所有订阅者从这里拿数据
        bus.publish(Topic::Speed, std::make_shared<DataPoint>(d));

        t += dt;
        next_tick += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(dt));
        std::this_thread::sleep_until(next_tick);
    }
}
