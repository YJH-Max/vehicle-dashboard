#pragma once
// 非阻塞 epoll 上报模块（单连接）
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <fcntl.h>

#include "blocking_queue.hpp"
#include "logger.hpp"

template <typename T>
class EpollReporter {
public:
    using MsgPtr = std::shared_ptr<const T>;
    using SubQueue = BlockingQueue<MsgPtr>;

    EpollReporter(SubQueue& q, std::string host, int port,
                  std::string (*toJson)(const T&))
        : queue_(q), host_(std::move(host)), port_(port), toJson_(toJson) {}

    ~EpollReporter() { stop(); }

    EpollReporter(const EpollReporter&) = delete;
    EpollReporter& operator=(const EpollReporter&) = delete;

    void start() {
        if (running_.exchange(true)) return;
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
    }

    std::uint64_t sent()       const noexcept { return sent_.load(); }
    std::uint64_t reconnects() const noexcept { return reconnects_.load(); }

private:
    void run() {
        epoll_fd_ = epoll_create1(0);
        if (epoll_fd_ < 0) {
            LOG_ERROR("[epoll] epoll_create1 failed");
            return;
        }

        int backoff_ms = 1000;
        constexpr int kMaxBackoffMs = 30000;

        while (running_.load(std::memory_order_relaxed)) {
            // 若无连接且退避到期 → 尝试连接
            if (sock_fd_ < 0) {
                auto now = std::chrono::steady_clock::now();
                if (now >= next_retry_) {
                    if (!tryConnect()) {
                        next_retry_ = now + std::chrono::milliseconds(backoff_ms);
                        backoff_ms = std::min(backoff_ms * 2, kMaxBackoffMs);
                        reconnects_.fetch_add(1);
                        // 退避期间抽干 net_q，避免队列堆积堵住 TopicBus
                        MsgPtr dummy;
                        while (queue_.try_consume(dummy)) {}
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        continue;
                    }
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
            }

            // 从 net_q 抽一批数据到用户态发送缓冲
            drainQueueToBuf();

            // 有数据要发但还没订阅 EPOLLOUT → 改订阅
            if (!send_buf_.empty() && !want_write_) {
                modifyEpoll(EPOLLIN | EPOLLOUT);
                want_write_ = true;
            }

            // epoll_wait：10ms 超时（兼顾 net_q 检查）
            epoll_event events[16];
            int n = epoll_wait(epoll_fd_, events, 16, 10);
            if (n < 0) {
                if (errno == EINTR) continue;
                LOG_ERROR("[epoll] epoll_wait failed");
                break;
            }

            for (int i = 0; i < n; ++i) {
                handleEvent(events[i].events, backoff_ms);
            }
        }

        closeSocket();
        if (epoll_fd_ >= 0) { ::close(epoll_fd_); epoll_fd_ = -1; }
        LOG_INFO("[epoll] stopped, sent=" + std::to_string(sent_.load()));
    }

    bool tryConnect() {
        sock_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (sock_fd_ < 0) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(static_cast<uint16_t>(port_));
        if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
            ::close(sock_fd_); sock_fd_ = -1; return false;
        }

        int ret = ::connect(sock_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (ret == 0) {
            connecting_ = false;
        } else if (errno == EINPROGRESS) {
            connecting_ = true;
        } else {
            ::close(sock_fd_); sock_fd_ = -1; return false;
        }

        // 注册到 epoll
        epoll_event ev{};
        ev.events  = EPOLLIN | EPOLLRDHUP | (connecting_ ? EPOLLOUT : 0);
        ev.data.fd = sock_fd_;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, sock_fd_, &ev) < 0) {
            ::close(sock_fd_); sock_fd_ = -1; return false;
        }
        want_write_ = connecting_;

        LOG_INFO("[epoll] connecting to " + host_ + ":" + std::to_string(port_));
        return true;
    }

    void handleEvent(uint32_t ev, int& backoff_ms) {
        // 错误/HUP → 关连接，退避重连
        if (ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
            int err = 0; socklen_t len = sizeof(err);
            getsockopt(sock_fd_, SOL_SOCKET, SO_ERROR, &err, &len);
            LOG_WARN(std::string("[epoll] connection lost: ")
                     + (err ? std::strerror(err) : "EOF"));
            closeSocket();
            next_retry_ = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(backoff_ms);
            backoff_ms = std::min(backoff_ms * 2, 30000);
            reconnects_.fetch_add(1);
            return;
        }

        // 连接建立（EPOLLOUT 第一次触发）
        if (connecting_ && (ev & EPOLLOUT)) {
            int err = 0; socklen_t len = sizeof(err);
            getsockopt(sock_fd_, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err != 0) {
                LOG_WARN(std::string("[epoll] connect failed: ") + std::strerror(err));
                closeSocket();
                next_retry_ = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(backoff_ms);
                backoff_ms = std::min(backoff_ms * 2, 30000);
                reconnects_.fetch_add(1);
                return;
            }
            connecting_ = false;
            backoff_ms = 1000;   // 连上了，重置退避
            LOG_INFO("[epoll] connected to " + host_ + ":" + std::to_string(port_));
        }

        // 可读：读掉服务器回包（本项目服务器不回，防御性读）
        if (ev & EPOLLIN) {
            char tmp[1024];
            while (true) {
                ssize_t r = ::recv(sock_fd_, tmp, sizeof(tmp), 0);
                if (r > 0) continue;
                if (r == 0 || (r < 0 && errno != EAGAIN)) { closeSocket(); }
                break;
            }
        }

        // 可写：冲刷用户态缓冲
        if ((ev & EPOLLOUT) && !connecting_) {
            flushSendBuf(backoff_ms);
        }
    }

    void flushSendBuf(int& backoff_ms) {
        while (!send_buf_.empty()) {
            ssize_t n = ::send(sock_fd_, send_buf_.data(), send_buf_.size(), MSG_NOSIGNAL);
            if (n > 0) {
                send_buf_.erase(0, static_cast<size_t>(n));
                sent_.fetch_add(1);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;   // 内核缓冲区满，等下次 EPOLLOUT
            }
            if (n < 0 && errno == EINTR) continue;
            LOG_WARN(std::string("[epoll] send error: ") + std::strerror(errno));
            closeSocket();
            next_retry_ = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(backoff_ms);
            backoff_ms = std::min(backoff_ms * 2, 30000);
            reconnects_.fetch_add(1);
            return;
        }
        // 缓冲空 → 注销 EPOLLOUT，避免 busy loop
        if (want_write_) {
            modifyEpoll(EPOLLIN);
            want_write_ = false;
        }
    }

    void drainQueueToBuf() {
        constexpr std::size_t kMaxBuf = 1 * 1024 * 1024;   // 1MB 上限
        MsgPtr msg;
        int budget = 128;
        while (budget-- > 0 && queue_.try_consume(msg)) {
            if (send_buf_.size() >= kMaxBuf) break;   // 缓冲满，丢新保稳
            send_buf_ += toJson_(*msg);
            send_buf_ += '\n';
        }
    }

    void modifyEpoll(uint32_t events) {
        if (sock_fd_ < 0) return;
        epoll_event ev{};
        ev.events  = events | EPOLLRDHUP;
        ev.data.fd = sock_fd_;
        epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, sock_fd_, &ev);
    }

    void closeSocket() {
        if (sock_fd_ >= 0) {
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, sock_fd_, nullptr);
            ::close(sock_fd_);
            sock_fd_ = -1;
        }
        connecting_ = false;
        want_write_ = false;
    }

    SubQueue& queue_;
    std::string host_;
    int port_;
    std::string (*toJson_)(const T&);

    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> sent_{0};
    std::atomic<std::uint64_t> reconnects_{0};

    int epoll_fd_ = -1;
    int sock_fd_  = -1;
    bool connecting_ = false;
    bool want_write_ = false;
    std::string send_buf_;
    std::chrono::steady_clock::time_point next_retry_;
};
