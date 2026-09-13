#include <cstdio>
#include <iostream>
#include <vector>

#include "data_pool.hpp"

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAILED @line " << __LINE__ << ": " #cond << "\n"; \
            return 1; \
        } \
    } while (0)

int main() {
    // 1. 空 DataPool：snapshot 应返回空历史
    {
        DataPool pool;
        auto snap = pool.snapshot();
        CHECK(snap.history.empty());
        CHECK(pool.totalConsumed() == 0);
    }

    // 2. 降采样：300 个点，间隔 1ms，窗口 100ms → 应只留 3~4 个
    {
        DataPool pool;
        for (int i = 0; i < 300; ++i) {
            DataPoint d;
            d.timestamp.ms = 1'000'000 + i * 1;   // 每 1ms 一条
            d.speed = 60.0 + i * 0.1;
            d.temp  = 70.0;
            pool.produce(d);
        }
        const auto n = pool.drain();
        CHECK(n == 300);

        auto snap = pool.snapshot();
        CHECK(snap.history.size() >= 2);
        CHECK(snap.history.size() <= 4);
    }

    // 3. history 上限：500 个点，每 200ms 一个 → history 最多 100
    {
        DataPool pool;
        for (int i = 0; i < 500; ++i) {
            DataPoint d;
            d.timestamp.ms = 1'000'000 + i * 200;
            d.speed = 60.0;
            d.temp  = 70.0;
            pool.produce(d);
        }
        pool.drain();
        auto snap = pool.snapshot();
        CHECK(snap.history.size() <= 100);
    }

    // 4. snapshot 的 latest 是最新一条
    {
        DataPool pool;
        DataPoint d1; d1.timestamp.ms = 100; d1.speed = 50; d1.temp = 60;
        DataPoint d2; d2.timestamp.ms = 200; d2.speed = 80; d2.temp = 90;
        pool.produce(d1);
        pool.produce(d2);
        pool.drain();
        auto snap = pool.snapshot();
        CHECK(snap.latest.speed == 80.0);
        CHECK(snap.latest.temp  == 90.0);
    }

    std::printf("All DataPool tests passed\n");
    return 0;
}
