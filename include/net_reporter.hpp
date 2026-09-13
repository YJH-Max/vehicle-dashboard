#pragma once
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

#include "blocking_queue.hpp"
#include "logger.hpp"

template <typename T>
class NetReporter {
public:
    using MsgPtr = std::shared_ptr<const T>;
    using SubQueue = BlockingQueue<MsgPtr>;

    NetReporter(SubQueue& q, std::string host, int port,
                std::string (*toJson)(const T&))
        : queue_(q), host_(std::move(host)), port_(port), toJson_(toJson) {}

    ~NetReporter() { stop(); }

    NetReporter(const NetReporter&) = delete;
    NetReporter& operator=(const NetReporter&) = delete;

    void start() {
        if (running_.exchange(true)) return;
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        queue_.stop();
        if (thread_.joinable()) thread_.join();
    }

    std::uint64_t sent()       const noexcept { return sent_.load(); }
    std::uint64_t reconnects() const noexcept { return reconnects_.load(); }

private:
    int connectToRemote() {
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        const std::string portStr = std::to_string(port_);

        if (getaddrinfo(host_.c_str(), portStr.c_str(), &hints, &res) != 0) {
            return -1;
        }
        int fd = -1;
        for (auto* p = res; p != nullptr; p = p->ai_next) {
            fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (fd < 0) continue;
            if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
            ::close(fd);
            fd = -1;
        }
        freeaddrinfo(res);
        return fd;
    }

    void run() {
        int backoff_ms = 1000;
        constexpr int kMaxBackoffMs = 30000;

        while (running_.load(std::memory_order_relaxed)) {
            int fd = connectToRemote();
            if (fd < 0) {
                LOG_WARN("[net] connect " + host_ + ":" + std::to_string(port_)
                         + " failed, retry in " + std::to_string(backoff_ms) + "ms");
                std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
                backoff_ms = std::min(backoff_ms * 2, kMaxBackoffMs);
                reconnects_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            LOG_INFO("[net] connected to " + host_ + ":" + std::to_string(port_));
            backoff_ms = 1000;   // 连上了，重置退避

            MsgPtr msg;
            bool send_failed = false;
            while (running_.load(std::memory_order_relaxed)) {
                if (!queue_.consume_blocking(msg, 1000)) continue;   // 超时无数据

                std::string line = toJson_(*msg) + "\n";
                ssize_t n = ::send(fd, line.data(), line.size(), MSG_NOSIGNAL);
                if (n < 0) {
                    LOG_WARN("[net] send failed: " + std::string(std::strerror(errno)));
                    send_failed = true;
                    break;
                }
                sent_.fetch_add(1, std::memory_order_relaxed);
            }
            ::close(fd);
            if (send_failed) {
                std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
                backoff_ms = std::min(backoff_ms * 2, kMaxBackoffMs);
                reconnects_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        LOG_INFO("[net] reporter stopped, sent=" + std::to_string(sent_.load()));
    }

    SubQueue& queue_;
    std::string host_;
    int port_;
    std::string (*toJson_)(const T&);

    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> sent_{0};
    std::atomic<std::uint64_t> reconnects_{0};
};
