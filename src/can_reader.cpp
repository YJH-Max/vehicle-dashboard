// 独立进程：从 vcan0 收帧，写入共享内存
// 用法: ./can_reader [接口=vcan0]
#include "shm_ring.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

constexpr const char* kShmName  = "/vehicle_dashboard_ring";
constexpr std::size_t kCapacity = 8192;   // 越大越抗抖动

// 信号处理：只设标志，不做其他事（信号上下文里不能做复杂操作）
static std::atomic<bool> g_stop{false};
static void onSignal(int) { g_stop.store(true); }

int main(int argc, char** argv) {
    const char* iface = argc > 1 ? argv[1] : "vcan0";

    // 注册信号处理——Ctrl-C 时设 g_stop，主循环检测后优雅退出
    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);

    // 1. 创建共享内存队列
    using Ring = shmring::ShmRing<shmring::DataPoint>;
    auto* ring = Ring::create(kShmName, kCapacity);
    if (!ring) {
        std::perror("[can_reader] shm create");
        return 1;
    }
    std::printf("[can_reader] shm created: /dev/shm%s capacity=%zu\n",
                kShmName, kCapacity);

    // 2. 打开 vcan0 raw socket
    int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0) { std::perror("[can_reader] socket"); return 1; }

    ifreq ifr{};
    std::strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
        std::fprintf(stderr, "[can_reader] 接口 %s 不存在\n", iface);
        return 1;
    }
    sockaddr_can addr{};
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::perror("[can_reader] bind"); return 1;
    }
    can_filter filt[2] = {{0x101, CAN_SFF_MASK}, {0x102, CAN_SFF_MASK}};
    setsockopt(s, SOL_CAN_RAW, CAN_RAW_FILTER, filt, sizeof(filt));

    std::printf("[can_reader] listening on %s, writing to shm\n", iface);
    std::fflush(stdout);

    // 3. 主循环：收帧 → 解包 → 合成 → 写 shm
    can_frame f;
    double last_speed = 0.0, last_temp = 70.0;
    std::uint64_t written = 0, dropped = 0;
    auto last_report = std::chrono::steady_clock::now();

    // poll 带 200ms 超时，让主循环能周期性检查 g_stop
    pollfd pfd{};
    pfd.fd     = s;
    pfd.events = POLLIN;

    while (!g_stop.load(std::memory_order_relaxed)) {
        int pr = ::poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;   // 被信号打断，回循环检查 g_stop
            std::perror("[can_reader] poll");
            break;
        }
        if (pr == 0) continue;              // 超时，回循环检查 g_stop
        if (!(pfd.revents & POLLIN)) continue;

        ssize_t n = read(s, &f, sizeof(f));
        if (n < (ssize_t)sizeof(can_frame)) continue;

        std::uint16_t raw =
            (std::uint16_t)f.data[0] | (std::uint16_t(f.data[1]) << 8);
        switch (f.can_id & CAN_SFF_MASK) {
            case 0x101: last_speed = raw / 10.0; break;
            case 0x102: last_temp  = raw / 10.0; break;
        }

        shmring::DataPoint d{};
        d.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        d.speed = last_speed;
        d.temp  = last_temp;

        if (ring->push(d)) {
            ++written;
        } else {
            ++dropped;  // 队列满，丢新保稳
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - last_report).count() >= 1.0) {
            last_report = now;
            std::printf("[can_reader] written=%llu dropped=%llu\n",
                        (unsigned long long)written,
                        (unsigned long long)dropped);
            std::fflush(stdout);
        }
    }

    // 4. 优雅退出：关 socket、释放 shm
    std::printf("\n[can_reader] 收到停止信号，清理中...\n");
    ::close(s);
    ring->close();       // munmap + shm_unlink
    std::printf("[can_reader] 已退出，written=%llu dropped=%llu\n",
                (unsigned long long)written,
                (unsigned long long)dropped);
    return 0;
}
