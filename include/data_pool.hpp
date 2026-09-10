#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "ring_buffer.hpp"

// 单帧车载数据
struct DataPoint {
    double timestamp;  // epoch 毫秒
    double speed;      // km/h
    double temp;       // °C
};

// HTTP 读侧的快照
struct Snapshot {
    DataPoint latest{};
    std::vector<DataPoint> history;
};

class DataPool {
public:
    DataPool(size_t ring_capacity = 4096, size_t history_cap = 100)
        : ring_(ring_capacity), history_cap_(history_cap) {}

    // ---- 生产侧（generator 线程独占）----
    bool produce(const DataPoint& d) { return ring_.push(d); }

    // ---- 消费侧（consumer 线程独占）：把环形缓冲区抽干一次 ----
    // 返回本次消费的条数
    size_t drain() {
        DataPoint d, last{};
        size_t n = 0;
        while (ring_.pop(d)) {
            ++n;
            last = d;
            // 降采样：每 100ms 数据时间取一个显示点 → 曲线每秒 10 个点
            if (d.timestamp - last_sample_ts_ >= sample_interval_) {
                last_sample_ts_ = d.timestamp;
                std::lock_guard<std::mutex> lk(mtx_);
                history_.push_back(d);
                if (history_.size() > history_cap_) history_.pop_front();
            }
        }
        if (n > 0) {
            {
                std::lock_guard<std::mutex> lk(mtx_);
                latest_ = last;
            }
            total_consumed_.fetch_add(n, std::memory_order_relaxed);
        }
        return n;
    }

    // ---- 读侧（HTTP 线程）----
    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return Snapshot{latest_, {history_.begin(), history_.end()}};
    }

    std::uint64_t totalConsumed() const {
        return total_consumed_.load(std::memory_order_relaxed);
    }

private:
    // 无锁区：高频数据面（生产者 → 消费者）
    RingBuffer<DataPoint> ring_;

    // 加锁区：低频显示面（消费者写，HTTP 读）
    mutable std::mutex mtx_;
    std::deque<DataPoint> history_;
    DataPoint latest_{};
    size_t history_cap_;

    double sample_interval_ = 100.0;  // 100ms 采一个显示点 (毫秒)
    double last_sample_ts_ = 0.0;

    std::atomic<std::uint64_t> total_consumed_{0};
};
