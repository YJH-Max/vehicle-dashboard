#pragma once
#include <chrono>
#include <condition_variable>
#include <mutex>

#include "mpmc_queue.hpp"

// BlockingQueue: 为 MPMC 无锁队列提供阻塞唤醒能力
// 分层设计:
//   - 数据路径: 底层 MpmcQueue 的 produce/consume 仍是无锁 CAS
//   - 等待路径: 队列空时消费者挂起在 condition_variable 上，真正 0 CPU
// 取舍说明: consume 在持锁状态下执行，避免 lost wakeup 竞态；
//   消费操作纳秒级，持锁时间极短，对多消费者串行化的影响可忽略。
template <typename T>
class BlockingQueue {
public:
    explicit BlockingQueue(std::size_t capacity) : queue_(capacity) {}

    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

    // 生产者: 非阻塞。满了返回 false (丢新保稳)。成功则唤醒一个等待者。
    bool produce(const T& v) {
        if (!queue_.produce(v)) return false;
        std::lock_guard<std::mutex> lk(mtx_);
        cv_.notify_one();
        return true;
    }

    // 非阻塞消费 (兼容旧调用)
    bool try_consume(T& out) {
        return queue_.consume(out);
    }

    // 阻塞消费:
    //   timeout_ms < 0  → 无限等待 (直到有数据或 stop)
    //   timeout_ms >= 0 → 最多等 timeout_ms 毫秒
    // 返回 false 表示 stop 或超时
    bool consume_blocking(T& out, int timeout_ms = -1) {
        std::unique_lock<std::mutex> lk(mtx_);
        while (true) {
            if (stop_) return false;

            // 持锁消费: 避免 lost wakeup。底层无锁，纳秒级。
            if (queue_.consume(out)) return true;

            // 队列空，等待生产者通知
            if (timeout_ms < 0) {
                cv_.wait(lk);
            } else {
                if (cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms))
                    == std::cv_status::timeout) {
                    // 超时前最后再试一次
                    return queue_.consume(out);
                }
            }
        }
    }

    // 优雅停机: 唤醒所有等待者，让它们退出 consume_blocking
    void stop() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
    }

    std::size_t capacity() const noexcept { return queue_.capacity(); }

private:
    MpmcQueue<T, YieldWait> queue_;   // 底层: 无锁底座
    std::mutex              mtx_;     // 等待路径专用，不参与数据路径
    std::condition_variable cv_;
    bool                    stop_ = false;   // 只在 mtx_ 保护下访问
};
