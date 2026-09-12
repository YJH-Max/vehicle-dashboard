#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <App.h>
#include "json.hpp"

#include "data_pool.hpp"
#include "data_generator.hpp"
#include "topic_bus.hpp"

using json = nlohmann::json;
using MsgPtr = TopicBus<DataPoint>::MessagePtr;

// ---------------- 基准模式（不变） ----------------
static int runBench1P1C() {
    DataPool pool;
    std::atomic<bool> running{true};
    constexpr auto kDuration = std::chrono::seconds(5);

    std::thread consumer([&] {
        while (running.load(std::memory_order_relaxed)) pool.drain();
    });

    const auto start = std::chrono::steady_clock::now();
    std::uint64_t pushed = 0;
    DataPoint d{Timestamp{}, 60.0, 50.0};

    while (std::chrono::steady_clock::now() - start < kDuration) {
        if (pool.produce(d)) ++pushed;
        else std::this_thread::yield();
    }

    running.store(false, std::memory_order_relaxed);
    consumer.join();

    const std::uint64_t consumed = pool.totalConsumed();
    std::printf("\n===== MPMC Benchmark (1P1C, 5 s) =====\n");
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

static json snapJson(const DataPoint& d) {
    return {{"timestamp", d.timestamp.ms},
            {"speed", d.speed},
            {"temp", d.temp}};
}

// ---------------- CAN 源线程 ----------------
static bool canSourceThread(DataPool& pool,
                            TopicBus<DataPoint>& bus,
                            const char* iface,
                            std::atomic<bool>& running) {
    int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0) { std::perror("[can] socket"); return false; }

    ifreq ifr{};
    std::strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
        std::fprintf(stderr, "[can] 接口 %s 不存在\n", iface);
        return false;
    }

    sockaddr_can addr{};
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::perror("[can] bind"); return false;
    }

    can_filter filt[2] = {{0x101, CAN_SFF_MASK}, {0x102, CAN_SFF_MASK}};
    setsockopt(s, SOL_CAN_RAW, CAN_RAW_FILTER, filt, sizeof(filt));

    std::printf("[can] 数据源: %s (0x101 车速 / 0x102 水温)\n", iface);
    std::fflush(stdout);

    can_frame f;
    double lastSpeed = 0.0, lastTemp = 70.0;
    while (running.load(std::memory_order_relaxed)) {
        const ssize_t n = read(s, &f, sizeof(f));
        if (n < (ssize_t)sizeof(can_frame)) continue;

        const std::uint16_t raw =
            (std::uint16_t)f.data[0] | (std::uint16_t(f.data[1]) << 8);

        switch (f.can_id & CAN_SFF_MASK) {
            case 0x101: lastSpeed = raw / 10.0; break;
            case 0x102: lastTemp  = raw / 10.0; break;
        }
        DataPoint d{Timestamp::now(), lastSpeed, lastTemp};

        pool.produce(d);
        bus.publish(Topic::Speed, std::make_shared<DataPoint>(d));
    }
    close(s);
    return true;
}

int main(int argc, char* argv[]) {
    if (argc > 1 && std::string(argv[1]) == "--bench") return runBench1P1C();

    DataPool pool;
    TopicBus<DataPoint> bus;
    std::atomic<bool> running{true};

    // ---- 数据源分流 ----
    std::thread dataThread([&] {
        if (argc > 2 && std::string(argv[1]) == "--can") {
            if (canSourceThread(pool, bus, argv[2], running)) return;
            std::fprintf(stderr, "[can] 启动失败，自动回退到内部模拟源\n");
        }
        dataGenerator(pool, bus, 1000, running);
    });

    // ---- 订阅者队列 + 注册 ----
    MpmcQueue<MsgPtr, YieldWait> ws_q(8192);
    MpmcQueue<MsgPtr, YieldWait> alarm_q(1024);
    auto h_ws    = bus.subscribe(Topic::Speed, &ws_q);
    auto h_alarm = bus.subscribe(Topic::Speed, &alarm_q);

    bus.start();

    // ---- uWS 初始化 ----
    struct PerSocketData {};
    uWS::App app;
    uWS::Loop* loop = uWS::Loop::get();

    app.get("/api/latest", [&pool](auto* res, auto* /*req*/) {
        auto snap = pool.snapshot();
        res->writeHeader("Content-Type", "application/json")->end(snapJson(snap.latest).dump());
    });

    app.get("/api/history", [&pool](auto* res, auto* /*req*/) {
        auto snap = pool.snapshot();
        json j = json::array();
        for (const auto& d : snap.history) j.push_back(snapJson(d));
        res->writeHeader("Content-Type", "application/json")->end(j.dump());
    });

    const std::string indexHtml = readFile("./www/index.html");
    app.get("/", [&indexHtml](auto* res, auto* /*req*/) {
        res->writeHeader("Content-Type", "text/html; charset=utf-8")->end(indexHtml);
    });

    app.ws<PerSocketData>("/ws", {
        .compression = uWS::SHARED_COMPRESSOR,
        .open        = [](auto* ws) { ws->subscribe("telemetry"); },
        .message     = [](auto*, std::string_view, uWS::OpCode) {},
        .close       = [](auto*, int, std::string_view) {}
    });

    // ---- 原 consumer：只做 pool 整理 + 打印（不再广播）----
    std::thread consumer([&] {
        auto last_report = std::chrono::steady_clock::now();
        std::uint64_t last_total = 0;
        while (running.load(std::memory_order_relaxed)) {
            pool.drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - last_report).count() >= 1.0) {
                last_report = now;
                const auto total = pool.totalConsumed();
                std::printf("[吞吐量] %llu 条/秒 | 累计 %llu 条 | WS推送 20Hz\n",
                            (unsigned long long)(total - last_total),
                            (unsigned long long)total);
                std::fflush(stdout);
                last_total = total;
            }
        }
    });

    // ---- 新增 ws_consumer：订阅者 → 20Hz 合并快照 → uWS 广播 ----
    std::thread ws_consumer([&] {
        MsgPtr msg;
        DataPoint snapshot{};
        auto next_pub = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);

        while (running.load(std::memory_order_relaxed)) {
            if (ws_q.consume(msg)) {
                snapshot = *msg;   // 只更新快照，不立即推送
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= next_pub) {
                next_pub += std::chrono::milliseconds(50);
                DataPoint s = snapshot;  // 拷贝一份供 lambda 捕获
                loop->defer([&app, s] {
                    app.publish("telemetry", snapJson(s).dump(), uWS::OpCode::TEXT);
                });
            }
        }
    });

    // ---- 新增 alarm_consumer：订阅者 → 实时判断告警 ----
    std::thread alarm_consumer([&] {
        MsgPtr msg;
        while (running.load(std::memory_order_relaxed)) {
            if (alarm_q.consume(msg)) {
                if (msg->speed > 130.0) {
                    std::printf("[ALARM] 车速 %.1f km/h 超阈值 130\n", msg->speed);
                    std::fflush(stdout);
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    });

    app.listen(8080, [](auto* token) {
        if (token) std::cout << "Server started: http://localhost:8080 (WS: /ws)\n";
        else { std::cerr << "8080 被占用，先 pkill dashboard\n"; std::exit(1); }
    });

    app.run();

    // ---- 停机顺序：先停数据源 → 停总线 → 停订阅者 → 停 consumer ----
    running.store(false);
    dataThread.join();
    bus.stop();
    ws_consumer.join();
    alarm_consumer.join();
    consumer.join();
    return 0;
}
