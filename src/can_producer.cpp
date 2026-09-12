// 模拟 ECU: 0x101 车速(uint16, 0.1km/h 分辨率) / 0x102 水温(uint16, 0.1°C)
// 用法: ./can_producer [接口=vcan0] [速率Hz=500]
#include <linux/can.h>
#include <linux/can/raw.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <thread>

int main(int argc, char** argv) {
    const char* iface = argc > 1 ? argv[1] : "vcan0";
    int rate = argc > 2 ? atoi(argv[2]) : 500;          // 车速帧频率

    int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0) { perror("socket"); return 1; }

    ifreq ifr{};
    std::strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) { perror("接口不存在(先跑 scripts/setup_vcan.sh)"); return 1; }

    sockaddr_can addr{};
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }

    std::printf("CAN 生产者 → %s | 0x101 车速 @%dHz | 0x102 水温 @%dHz\n",
                iface, rate, rate / 10);

    can_frame sf{}, tf{};
    sf.can_id = 0x101; sf.can_dlc = 2;
    tf.can_id = 0x102; tf.can_dlc = 2;

    double t = 0; long tick = 0;
    auto period = std::chrono::microseconds(1'000'000 / rate);
    auto next = std::chrono::steady_clock::now();

    while (true) {
        next += period;
        
        std::this_thread::sleep_until(next);
        t += 1.0 / rate;

        double speed = 60 + 30 * std::sin(t / 5.0) + 5 * std::sin(t * 1.7);
        double temp  = 70 + 8  * std::sin(t / 30.0);
        uint16_t s10 = (uint16_t)std::lround(speed * 10);
        uint16_t t10 = (uint16_t)std::lround(temp * 10);

        sf.data[0] = s10 & 0xFF; sf.data[1] = s10 >> 8;   // 小端
        tf.data[0] = t10 & 0xFF; tf.data[1] = t10 >> 8;

        (void)!write(s, &sf, sizeof(sf));
        if (tick % 10 == 0) (void)!write(s, &tf, sizeof(tf));    // 水温 1/10 速率
        ++tick;
    }
}
