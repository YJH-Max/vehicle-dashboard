// epoll 上报模块自包含测试：内嵌 mini TCP server，不依赖外部进程
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "blocking_queue.hpp"
#include "epoll_reporter.hpp"
#include "logger.hpp"

struct Sample { double speed; };

static std::atomic<bool>          g_srv_running{true};
static std::atomic<int>           g_srv_port{0};
static std::atomic<std::uint64_t> g_bytes_received{0};
static int                        g_cli_fd = -1;

// 极简 TCP 服务器：接受一个连接，读光所有字节
static void miniServer() {
    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;   // 让内核分配
    if (::bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0) { ::close(srv); return; }
    ::listen(srv, 1);

    socklen_t len = sizeof(addr);
    ::getsockname(srv, (sockaddr*)&addr, &len);
    g_srv_port.store(ntohs(addr.sin_port));

    int cli = ::accept(srv, nullptr, nullptr);
    if (cli >= 0) {
        g_cli_fd = cli;
        char buf[4096];
        while (g_srv_running.load()) {
            ssize_t n = ::recv(cli, buf, sizeof(buf), 0);
            if (n <= 0) break;
            g_bytes_received.fetch_add(static_cast<std::uint64_t>(n));
        }
        ::close(cli);
    }
    ::close(srv);
}

int main() {
    Logger::instance().init("/tmp/epoll_test.log", LogLevel::INFO);

    std::thread srv(miniServer);
    srv.detach();

    // 等服务器端口就绪
    for (int i = 0; i < 100 && g_srv_port.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (g_srv_port.load() == 0) {
        std::fprintf(stderr, "FAIL: mini server 未启动\n");
        return 1;
    }
    const int port = g_srv_port.load();

    BlockingQueue<std::shared_ptr<const Sample>> q(8192);
    auto jsonFn = [](const Sample& s) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "{\"speed\":%.1f}", s.speed);
        return std::string(buf);
    };

    EpollReporter<Sample> rep(q, "127.0.0.1", port, jsonFn);
    rep.start();

    // 生产 100 条
    for (int i = 0; i < 100; ++i) {
        auto s = std::make_shared<Sample>(Sample{60.0 + i * 0.01});
        while (!q.produce(s)) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 等发送完成
    std::this_thread::sleep_for(std::chrono::seconds(2));

    const std::uint64_t sent = rep.sent();
    const std::uint64_t bytes = g_bytes_received.load();
    rep.stop();
    g_srv_running.store(false);

    std::printf("sent=%llu reconnects=%llu bytes_received=%llu\n",
                (unsigned long long)sent,
                (unsigned long long)rep.reconnects(),
                (unsigned long long)bytes);

    if (sent == 0) {
        std::fprintf(stderr, "FAIL: sent == 0\n");
        return 1;
    }
    if (bytes == 0) {
        std::fprintf(stderr, "FAIL: server 没收到任何字节\n");
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
