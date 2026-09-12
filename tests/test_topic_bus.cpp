#include <cstdio>
#include <thread>
#include <chrono>
#include "topic_bus.hpp"

#include "blocking_queue.hpp"
struct Sample { double speed; double temp; };

int main() {
    TopicBus<Sample> bus;

    using MsgPtr = TopicBus<Sample>::MessagePtr;
    BlockingQueue<MsgPtr> speed_q(4096);
    BlockingQueue<MsgPtr> all_q(4096);

    auto h1 = bus.subscribe(Topic::Speed, &speed_q);   // 只订车速
    auto h2 = bus.subscribe(Topic::Speed, &all_q);     // 也订车速
    auto h3 = bus.subscribe(Topic::Temp,  &all_q);     // 订水温

    int cb_count = 0;
    auto h4 = bus.subscribe(Topic::Speed, [&](const Sample&) { ++cb_count; });

    bus.start();

    for (int i = 0; i < 100; ++i) {
        bus.publish(Topic::Speed, std::make_shared<Sample>(Sample{60.0 + i, 0}));
        bus.publish(Topic::Temp,  std::make_shared<Sample>(Sample{0, 70.0 + i}));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 显式注销 h1 再验证后续推送不影响它
    h1.reset();
    for (int i = 0; i < 5; ++i) {
        bus.publish(Topic::Speed, std::make_shared<Sample>(Sample{999, 0}));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    bus.stop();

    int speed_n = 0, all_n = 0;
    MsgPtr p;
    while (speed_q.try_consume(p)) ++speed_n;
    while (all_q.try_consume(p))   ++all_n;

    std::printf("speed_q: %d (expect 100)\n", speed_n);
    std::printf("all_q:   %d (expect 205)\n", all_n);
    std::printf("callback:%d (expect 105)\n", cb_count);

    bool ok = (speed_n == 100) && (all_n == 205) && (cb_count == 105);
    std::printf("result : %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
