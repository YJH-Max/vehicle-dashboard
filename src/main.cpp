/*#include <iostream>
#include "httplib.h"

int main(){
    httplib::Server svr;

    svr.Get("/api/ping", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"ok","version":"v0.1"})", "\n","application/json");
    });

    svr.set_mount_point("/", "./www");

    std::cout << "Server started at http://localhost:8080" << std::endl;
    svr.listen("0.0.0.0", 8080);
    return 0;
}
*/
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#include "httplib.h"
#include "json.hpp"

#include "data_pool.hpp"
#include "data_generator.hpp"

using json = nlohmann::json;

// ---------------- 基准模式：./dashboard --bench ----------------
static int runBench() {
    DataPool pool;
    std::atomic<bool> running{true};
    constexpr auto kDuration = std::chrono::seconds(5);

    std::thread consumer([&] {
        while (running.load(std::memory_order_relaxed)) {
            pool.drain();
        }
    });

    const auto start = std::chrono::steady_clock::now();
    std::uint64_t pushed = 0;
    DataPoint d{0.0, 60.0, 50.0};  // 基准模式只测队列，数据内容无所谓

    while (std::chrono::steady_clock::now() - start < kDuration) {
        if (pool.produce(d)) {
            ++pushed;
        } else {
            std::this_thread::yield();  // 缓冲区满：让出 CPU 给消费者
        }
    }

    running.store(false, std::memory_order_relaxed);
    consumer.join();

    const std::uint64_t consumed = pool.totalConsumed();
    std::printf("\n===== SPSC RingBuffer Benchmark (5 s) =====\n");
    std::printf("pushed    : %llu\n", (unsigned long long)pushed);
    std::printf("consumed  : %llu\n", (unsigned long long)consumed);
    std::printf("throughput: %.2f million items/sec\n", consumed / 5.0 / 1e6);
    return 0;
}

// ---------------- 正常模式：数据管线 + HTTP 服务 ----------------
int main(int argc, char* argv[]) {
    if (argc > 1 && std::string(argv[1]) == "--bench") {
        return runBench();
    }

    DataPool pool;
    std::atomic<bool> running{true};

    // 生产者：1000 帧/秒
    std::thread producer(dataGenerator, std::ref(pool), 1000, std::ref(running));

    // 消费者：持续抽干环形缓冲区 + 每 1 秒打印吞吐量
    std::thread consumer([&] {
        auto last_report = std::chrono::steady_clock::now();
        std::uint64_t last_total = 0;
        while (running.load(std::memory_order_relaxed)) {
            pool.drain();
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - last_report).count() >= 1.0) {
                last_report = now;
                const std::uint64_t total = pool.totalConsumed();
                std::printf("[吞吐量] %llu 条/秒 | 累计 %llu 条\n",
                            (unsigned long long)(total - last_total),
                            (unsigned long long)total);
                last_total = total;
            }
        }
    });

    httplib::Server svr;

    svr.Get("/api/latest", [&](const httplib::Request&, httplib::Response& res) {
        auto snap = pool.snapshot();
        json j = {{"timestamp", snap.latest.timestamp},
                  {"speed", snap.latest.speed},
                  {"temp", snap.latest.temp}};
        res.set_content(j.dump(), "application/json");
    });

    svr.Get("/api/history", [&](const httplib::Request&, httplib::Response& res) {
        auto snap = pool.snapshot();
        json j = json::array();
        for (const auto& d : snap.history) {
            j.push_back({{"timestamp", d.timestamp},
                         {"speed", d.speed},
                         {"temp", d.temp}});
        }
        res.set_content(j.dump(), "application/json");
    });

    svr.set_mount_point("/", "./www");

    std::cout << "Server started at http://localhost:8080" << std::endl;
    svr.listen("0.0.0.0", 8080);

    running.store(false);
    producer.join();
    consumer.join();
    return 0;
}
