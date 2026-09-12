#pragma once
#include <cstdint>
#include <chrono>

// epoch 毫秒时间戳 —— 全项目唯一的时间口径
// 规则：禁止裸 double/uint64_t 表达时间，必须经过此类型
struct Timestamp {
    std::uint64_t ms{0};

    // 全项目唯一的"当前时间"出口
    static Timestamp now() noexcept {
        return {static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count())};
    }
};
