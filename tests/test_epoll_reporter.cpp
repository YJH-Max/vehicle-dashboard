// 独立验证 epoll 上报：从队列拿数据，通过 epoll 非阻塞发到 mock server
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include "blocking_queue.hpp"
#include "epoll_reporter.hpp"
#include "logger.hpp"

struct Sample { double speed; };

int main() {
    Logger::instance().init("/tmp/epoll_test.log", LogLevel::INFO);
    BlockingQueue<std::shared_ptr<const Sample>> q(8192);

    auto jsonFn = [](const Sample& s) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "{\"speed\":%.1f}", s.speed);
        return std::string(buf);
    };

    EpollReporter<Sample> rep(q, "127.0.0.1", 9000, jsonFn);
    rep.start();

    // 生产 1000 条，模拟 20Hz
    for (int i = 0; i < 1000; ++i) {
        auto s = std::make_shared<Sample>(Sample{60.0 + i * 0.01});
        while (!q.produce(s)) {
            // 用一个简单自旋——测试代码不追求高性能
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::printf("producer done, waiting 2s for epoll to flush\n");
    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::printf("sent=%llu reconnects=%llu\n",
                (unsigned long long)rep.sent(),
                (unsigned long long)rep.reconnects());
    rep.stop();
    return 0;
}
