#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "blocking_queue.hpp"

enum class LogLevel : std::uint8_t {
    DEBUG = 0,
    INFO  = 1,
    WARN  = 2,
    ERROR = 3,
};

inline const char* logLevelName(LogLevel lv) {
    switch (lv) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
    }
    return "?";
}

struct LogEntry {
    std::uint64_t timestamp_ms;
    std::uint64_t thread_id;
    LogLevel      level;
    std::string   message;
};

// 全局单例。主线程只调 log() 入队，落盘由独立线程做。
class Logger {
public:
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    // 启动日志系统，打开文件并启动落盘线程
    bool init(const std::string& filepath, LogLevel min_level = LogLevel::DEBUG) {
        if (running_.exchange(true)) return true;   // 已初始化
        file_.open(filepath, std::ios::app);
        if (!file_.is_open()) {
            running_.store(false);
            return false;
        }
        min_level_ = min_level;
        flush_thread_ = std::thread([this] { flushLoop(); });
        return true;
    }

    // 关闭日志系统：停队列，等落盘线程退出
    void shutdown() {
        if (!running_.exchange(false)) return;
        queue_.stop();
        if (flush_thread_.joinable()) flush_thread_.join();
        if (file_.is_open()) file_.close();
    }

    // 主线程接口：只入队，不碰磁盘
    void log(LogLevel lv, std::string msg) {
        if (!running_.load(std::memory_order_relaxed)) return;
        if (static_cast<int>(lv) < static_cast<int>(min_level_)) return;

        auto e = std::make_shared<LogEntry>();
        e->timestamp_ms = nowMs();
        e->thread_id    = threadId();
        e->level        = lv;
        e->message      = std::move(msg);

        // 队列满则丢弃（丢新保稳），绝不阻塞业务
        queue_.produce(e);
    }

    ~Logger() { shutdown(); }

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    static std::uint64_t nowMs() {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }

    static std::uint64_t threadId() {
        return static_cast<std::uint64_t>(
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
    }

    static std::string format(const LogEntry& e) {
        // 把 ms 时间戳转成 yyyy-MM-dd HH:mm:ss.SSS
        std::time_t t = static_cast<std::time_t>(e.timestamp_ms / 1000);
        std::tm tm_buf{};
        localtime_r(&t, &tm_buf);
        char timebuf[64];
        std::snprintf(timebuf, sizeof(timebuf),
                      "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                      tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                      tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                      static_cast<int>(e.timestamp_ms % 1000));

        std::ostringstream ss;
        ss << '[' << timebuf << "] ["
           << logLevelName(e.level) << "] [tid="
           << e.thread_id << "] " << e.message;
        return ss.str();
    }

    void flushLoop() {
        std::shared_ptr<const LogEntry> e;
        while (true) {
            if (!queue_.consume_blocking(e, 500)) {
                if (!running_.load(std::memory_order_relaxed)) break;
                file_.flush();
                continue;
            }
            file_ << format(*e) << '\n';
        }
        // 退出前把剩余日志刷出去
        while (queue_.try_consume(e)) {
            file_ << format(*e) << '\n';
        }
        file_.flush();
    }

    BlockingQueue<std::shared_ptr<const LogEntry>> queue_{8192};
    std::thread      flush_thread_;
    std::ofstream    file_;
    std::atomic<bool> running_{false};
    LogLevel         min_level_{LogLevel::DEBUG};
};

// ---------- 宏：调用方只需 LOG_INFO("...") ----------
#define LOG_DEBUG(msg) ::Logger::instance().log(LogLevel::DEBUG, (msg))
#define LOG_INFO(msg)  ::Logger::instance().log(LogLevel::INFO,  (msg))
#define LOG_WARN(msg)  ::Logger::instance().log(LogLevel::WARN,  (msg))
#define LOG_ERROR(msg) ::Logger::instance().log(LogLevel::ERROR, (msg))
