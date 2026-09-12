#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "mpmc_queue.hpp"
#include "blocking_queue.hpp"

enum class Topic : std::uint8_t {
    Speed,
    Temp,
    Alarm,
};

inline const char* topicName(Topic t) {
    switch (t) {
        case Topic::Speed: return "Speed";
        case Topic::Temp:  return "Temp";
        case Topic::Alarm: return "Alarm";
    }
    return "Unknown";
}

template <typename T>
class TopicBus {
public:
    // 阶段2：单进程零拷贝
    // 阶段3：替换为 ShmPtr<const T>（基于 offset_ptr 或 uint32_t 偏移量）
    using MessagePtr = std::shared_ptr<const T>;
    using SubQueue   = BlockingQueue<MessagePtr>;
    using Callback   = std::function<void(const T&)>;

    // RAII 句柄：析构自动注销
    class SubscriptionHandle {
    public:
        SubscriptionHandle() = default;
        SubscriptionHandle(TopicBus* bus, std::uint64_t id)
            : bus_(bus), id_(id) {}
        ~SubscriptionHandle() { reset(); }
        SubscriptionHandle(SubscriptionHandle&& o) noexcept
            : bus_(o.bus_), id_(o.id_) { o.bus_ = nullptr; o.id_ = 0; }
        SubscriptionHandle& operator=(SubscriptionHandle&& o) noexcept {
            if (this != &o) { reset(); bus_ = o.bus_; id_ = o.id_; o.bus_ = nullptr; o.id_ = 0; }
            return *this;
        }
        SubscriptionHandle(const SubscriptionHandle&) = delete;
        SubscriptionHandle& operator=(const SubscriptionHandle&) = delete;
        void reset();
    private:
        TopicBus* bus_ = nullptr;
        std::uint64_t id_ = 0;
    };

    TopicBus() = default;
    ~TopicBus() { stop(); }

    TopicBus(const TopicBus&) = delete;
    TopicBus& operator=(const TopicBus&) = delete;

    SubscriptionHandle subscribe(Topic t, SubQueue* queue) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto id = next_id_++;
        queues_[t].push_back({id, queue});
        return SubscriptionHandle(this, id);
    }

    SubscriptionHandle subscribe(Topic t, Callback cb) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto id = next_id_++;
        callbacks_[t].push_back({id, std::move(cb)});
        return SubscriptionHandle(this, id);
    }

    void unsubscribe(std::uint64_t id) {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& kv : queues_) {
            auto& v = kv.second;
            for (auto it = v.begin(); it != v.end(); ++it) {
                if (it->id == id) { v.erase(it); return; }
            }
        }
        for (auto& kv : callbacks_) {
            auto& v = kv.second;
            for (auto it = v.begin(); it != v.end(); ++it) {
                if (it->id == id) { v.erase(it); return; }
            }
        }
    }

    // 发布：入队即返回，分发由独立线程异步完成
    bool publish(Topic t, MessagePtr msg) {
        return bus_queue_.produce(Envelope{t, std::move(msg)});
    }

    void start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) return;
        dispatch_thread_ = std::thread([this] { dispatchLoop(); });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (dispatch_thread_.joinable()) dispatch_thread_.join();
    }

    std::uint64_t dispatched() const noexcept {
        return dispatched_.load(std::memory_order_relaxed);
    }

private:
    struct Envelope {
        Topic      topic;
        MessagePtr msg;
    };
    struct QueueEntry {
        std::uint64_t id;
        SubQueue* queue;
    };
    struct CallbackEntry {
        std::uint64_t id;
        Callback cb;
    };

    void dispatchLoop() {
        Envelope env;
        while (running_.load(std::memory_order_relaxed)) {
            if (bus_queue_.consume(env)) {
                deliver(env);
                dispatched_.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        while (bus_queue_.consume(env)) {
            deliver(env);
            dispatched_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void deliver(const Envelope& env) {
        // 快照：持锁只拷贝订阅者列表本身（vector<Entry>），立刻释放
        std::vector<QueueEntry>    qcopy;
        std::vector<CallbackEntry> ccopy;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto qit = queues_.find(env.topic);
            if (qit != queues_.end()) qcopy = qit->second;
            auto cit = callbacks_.find(env.topic);
            if (cit != callbacks_.end()) ccopy = cit->second;
        }
        // 锁外执行：不会死锁，回调可以再调用 subscribe/unsubscribe
        for (auto& e : qcopy) {
            // 队列满则丢弃这一条（丢新保稳），不阻塞分发线程
            e.queue->produce(env.msg);
        }
        for (auto& e : ccopy) {
            e.cb(*env.msg);
        }
    }

    MpmcQueue<Envelope, YieldWait> bus_queue_{4096};

    std::unordered_map<Topic, std::vector<QueueEntry>>    queues_;
    std::unordered_map<Topic, std::vector<CallbackEntry>> callbacks_;
    mutable std::mutex mtx_;
    std::uint64_t next_id_{1};

    std::thread       dispatch_thread_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> dispatched_{0};
};

template <typename T>
void TopicBus<T>::SubscriptionHandle::reset() {
    if (bus_) { bus_->unsubscribe(id_); bus_ = nullptr; id_ = 0; }
}
