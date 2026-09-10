// 第4步：数据源 = SocketCAN(--can vcan0) 或 内部模拟(缺省兜底) —— uWS 静态页 + REST + WS推送(20Hz)
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

#include <linux/can.h>        // SocketCAN：内核 CAN 总线原生接口
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

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

// ================ 第4步新增：CAN 数据源 ================
// vcan0 收 0x101(车速)/0x102(水温) → 小端解包 → DataPool
// 返回 false = 总线不可用(调用方回退模拟源)；返回 true = 正常运行过
static bool canSourceThread(DataPool& pool, const char* iface,
                            std::atomic<bool>& running) {
    int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0) { std::perror("[can] socket"); return false; }

    ifreq ifr{};
    std::strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
        std::fprintf(stderr, "[can] 接口 %s 不存在（先跑 scripts/setup_vcan.sh）\n", iface);
        return false;
    }

    sockaddr_can addr{};
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::perror("[can] bind"); return false;
    }

    // 内核层过滤：只放行这两个 ID，其余报文根本不进用户态（省 CPU）
    can_filter filt[2] = {{0x101, CAN_SFF_MASK}, {0x102, CAN_SFF_MASK}};
    setsockopt(s, SOL_CAN_RAW, CAN_RAW_FILTER, filt, sizeof(filt));

    std::printf("[can] 数据源: %s (0x101 车速 / 0x102 水温)\n", iface);
    std::fflush(stdout);

    can_frame f;
    double lastSpeed = 0.0, lastTemp = 70.0;   // 两个 ID 分帧到达，各持最新值
    while (running.load(std::memory_order_relaxed)) {
        const ssize_t n = read(s, &f, sizeof(f));
        if (n < (ssize_t)sizeof(can_frame)) continue;

        const double ts = std::chrono::duration<double, std::milli>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        const std::uint16_t raw =
            (std::uint16_t)f.data[0] | (std::uint16_t(f.data[1]) << 8);  // 小端

        switch (f.can_id & CAN_SFF_MASK) {
            case 0x101: lastSpeed = raw / 10.0; break;   // 0.1 km/h 分辨率
            case 0x102: lastTemp  = raw / 10.0; break;   // 0.1 °C  分辨率
        }
        pool.produce({ts, lastSpeed, lastTemp});         // 每帧产出一条 → ~550 条/秒
    }
    close(s);
    return true;
}
// ======================================================

// ---------------- 正常模式 ----------------
int main(int argc, char* argv[]) {
    if (argc > 1 && std::string(argv[1]) == "--bench") return runBench();

    DataPool pool;
    std::atomic<bool> running{true};

    // ================ 第4步修改：数据源分流 ================
    // ./dashboard --can vcan0  → SocketCAN 总线源
    // ./dashboard              → 内部模拟 1000Hz（零配置兜底；总线故障也自动回退到这）
    std::thread dataThread([&] {
        if (argc > 2 && std::string(argv[1]) == "--can") {
            if (canSourceThread(pool, argv[2], running)) return;
            std::fprintf(stderr, "[can] 启动失败，自动回退到内部模拟源\n");
        }
        dataGenerator(pool, 1000, running);
    });
    // ======================================================

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
    dataThread.join();
    consumer.join();
    return 0;
}
