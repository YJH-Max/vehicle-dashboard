// 第2步：uWebSockets 版 —— 静态页 + REST + WebSocket 推送(20Hz)
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include <App.h>              // uWebSockets
#include "json.hpp"

#include "data_pool.hpp"
#include "data_generator.hpp"

using json = nlohmann::json;

// ---------------- 基准模式：./dashboard --bench（原样保留） ----------------
static int runBench() {
    DataPool pool;
    std::atomic<bool> running{true};
    constexpr auto kDuration = std::chrono::seconds(5);

    std::thread consumer([&] {
        while (running.load(std::memory_order_relaxed)) pool.drain();
    });

    const auto start = std::chrono::steady_clock::now();
    std::uint64_t pushed = 0;
    DataPoint d{0.0, 60.0, 50.0};

    while (std::chrono::steady_clock::now() - start < kDuration) {
        if (pool.produce(d)) ++pushed;
        else std::this_thread::yield();
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

static std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss; ss << in.rdbuf();
    return ss.str();
}

static json latestJson(DataPool& pool) {
    auto snap = pool.snapshot();
    return {{"timestamp", snap.latest.timestamp},
            {"speed", snap.latest.speed},
            {"temp", snap.latest.temp}};
}

// ---------------- 正常模式 ----------------
int main(int argc, char* argv[]) {
    if (argc > 1 && std::string(argv[1]) == "--bench") return runBench();

    DataPool pool;
    std::atomic<bool> running{true};

    // 生产者：1000 帧/秒（不变）
    std::thread producer(dataGenerator, std::ref(pool), 1000, std::ref(running));

    struct PerSocketData {};
    uWS::App app;
    uWS::Loop* loop = uWS::Loop::get();   // 主线程 = 事件循环线程

    // REST 接口（语义与 httplib 版一致）
    app.get("/api/latest", [&pool](auto* res, auto* /*req*/) {
        res->writeHeader("Content-Type", "application/json")->end(latestJson(pool).dump());
    });

    app.get("/api/history", [&pool](auto* res, auto* /*req*/) {
        auto snap = pool.snapshot();
        json j = json::array();
        for (const auto& d : snap.history)
            j.push_back({{"timestamp", d.timestamp}, {"speed", d.speed}, {"temp", d.temp}});
        res->writeHeader("Content-Type", "application/json")->end(j.dump());
    });

    // 静态页：uWS 没有挂载目录功能，启动时读进内存
    const std::string indexHtml = readFile("./www/index.html");
    app.get("/", [&indexHtml](auto* res, auto* /*req*/) {
        res->writeHeader("Content-Type", "text/html; charset=utf-8")->end(indexHtml);
    });

    // WebSocket：/ws，客户端订阅 "telemetry" 主题
    app.ws<PerSocketData>("/ws", {
        .compression = uWS::SHARED_COMPRESSOR,
        .open        = [](auto* ws) { ws->subscribe("telemetry"); },
        .message     = [](auto*, std::string_view, uWS::OpCode) {},  // 前端只收不发
        .close       = [](auto*, int, std::string_view) {}
    });

    // 消费者：抽干缓冲区 + 每 50ms(20Hz) 广播一次 + 每秒打印吞吐量
    std::thread consumer([&] {
        auto last_report = std::chrono::steady_clock::now();
        auto next_pub    = last_report + std::chrono::milliseconds(50);
        std::uint64_t last_total = 0;

        while (running.load(std::memory_order_relaxed)) {
            pool.drain();
            const auto now = std::chrono::steady_clock::now();

            if (now >= next_pub) {
                next_pub += std::chrono::milliseconds(50);
                loop->defer([&app, &pool] {   // 切回事件循环线程再广播（线程安全）
                    app.publish("telemetry", latestJson(pool).dump(), uWS::OpCode::TEXT);
                });
            }

            if (std::chrono::duration<double>(now - last_report).count() >= 1.0) {
                last_report = now;
                const std::uint64_t total = pool.totalConsumed();
                std::printf("[吞吐量] %llu 条/秒 | 累计 %llu 条 | WS推送 20Hz\n",
                            (unsigned long long)(total - last_total),
                            (unsigned long long)total);
                std::fflush(stdout);
                last_total = total;
            }
        }
    });

    app.listen(8080, [](auto* token) {
        if (token) std::cout << "Server started: http://localhost:8080 (WS: /ws)\n";
        else { std::cerr << "8080 被占用，先 pkill dashboard\n"; std::exit(1); }
    });

    app.run();   // 阻塞主线程

    running.store(false);
    producer.join();
    consumer.join();
    return 0;
}
