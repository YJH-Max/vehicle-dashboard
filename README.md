# 🚗 车载实时数据监控仪表盘（C++ / SocketCAN / WebSocket）

![CI](https://github.com/YJH-Max/vehicle-dashboard/actions/workflows/ci.yml/badge.svg)

从内核 CAN 总线到浏览器曲线的完整实时链路：**SocketCAN 收帧 → 无锁 SPSC 队列 → uWebSockets 推送 → ECharts 可视化**。


## 一键安装依赖

    ./scripts/install_deps.sh

## Docker 构建

    docker build -t vehicle-dashboard .
    docker run --rm -p 8080:8080 vehicle-dashboard /app/dashboard


## 架构

    ┌──────────────┐  CAN 2.0 帧   ┌─────────────────────────┐
    │ can_producer │ ─550 帧/秒──▶ │  vcan0（SocketCAN 内核） │
    └──────────────┘               └────────────┬────────────┘
                                            │ PF_CAN RAW + 内核层 ID 过滤
                                                ▼
                             ┌──────────────────────────────┐
                             │  dashboard（C++/uWebSockets） │
                             │  can_source_thread 小端解包    │
                             │          │ produce()         │
                             │          ▼                   │
                             │   SPSC 无锁 RingBuffer        │
                             │          │ drain()           │
                             │    ┌─────┴──────┐            │
                             │    ▼            ▼            │
                             │ REST API     WS 广播 20Hz     │
                             └──────┬──────────┬───────────┘
                                    ▼          ▼
                             /api/history  浏览器 ECharts 曲线

## 协议（模拟真实 ECU 报文）

| ID | 周期 | 信号 | 字节布局 | 解码 |
|----|------|------|----------|------|
| 0x101 | 2 ms（500Hz） | 车速 | data[0..1] 小端 uint16 | raw / 10 = km/h（25~95 正弦） |
| 0x102 | 20 ms（50Hz） | 水温 | data[0..1] 小端 uint16 | raw / 10 = °C（70~80 正弦） |

## 快速开始

    # 依赖：g++ / cmake / can-utils / uWebSockets / nlohmann/json

    # 终端 1：拉起虚拟总线 + 生产者
    ./scripts/setup_vcan.sh
    ./build/can_producer

    # 终端 2：启动服务（--can 指定总线源）
    ./build/dashboard --can vcan0

    # 浏览器打开 http://localhost:8080

- ./build/dashboard（不带参数）→ 内部模拟源 1000Hz，零配置兜底；总线不可用时自动回退
- ./build/dashboard --bench → 队列 5 秒基准测试

## 演示：总线故障冻结实验

1. 掐掉 can_producer（Ctrl-C）→ 曲线**冻结在最后有效值**，页面保持在线
2. 重启 ./build/can_producer → 曲线**立刻复活**，正弦波恢复

实测：车速冻结在 32.6 km/h 约 40 秒 → 重启后 62.6 → 92.6 峰值 → 完整周期恢复。

## 性能数据

| 指标 | 数值 |
|------|------|
| 队列吞吐（--bench 实测，SPSC 基线） | 7.46 million items/sec（5 秒 3729 万条，推入≈消费零丢失） |
| CAN 链路持续吞吐 | ~549 条/秒（500 车速 + 50 水温） |
| 长稳测试 | 600 万+ 消息 / 连续 100 分钟 / 零崩溃 |
| 推送频率 | WebSocket 20Hz（与服务端节拍解耦，客户端数量不影响） |

## 关键实现

- **无锁 SPSC RingBuffer**：head/tail 原子索引 + cache line 对齐，单生产者单消费者零竞争；写满时 yield 背压而非丢数据
- **内核态报文过滤**：CAN_RAW_FILTER 只放行 0x101/0x102，无关 ID 不进用户态
- **线程安全广播**：uWS 非线程安全，工作线程通过 loop->defer() 把 publish 投递回事件循环线程
- **数据源可插拔**：DataPool 为唯一契约，CAN / 模拟双后端，前端零改动——架构解耦的实测验证

## 目录结构

    ├── src/main.cpp            # 服务端：数据源分流 + REST + WS
    ├── src/can_producer.cpp    # CAN 生产者（模拟 ECU）
    ├── include/data_pool.hpp   # SPSC 无锁队列
    ├── include/data_generator.hpp
    ├── scripts/setup_vcan.sh   # vcan0 一键配置（幂等）
    └── www/index.html          # 前端（ECharts）

## 面试常见问答

**Q: 为什么自己写 SPSC 无锁队列而不 mutex？**
单生产者单消费者是固定拓扑，两个原子索引即可零锁同步；mutex 在高频下有锁竞争和上下文切换开销，尾延迟不可控。

**Q: 550 帧/秒并不高，无锁队列是否过度设计？**
当前流量确实小，但基准测试给出的是**设计余量**——真实整车多 ECU 汇聚后帧率会放大数个量级，数据通道不能成为第一个瓶颈。

**Q: 冻结实验证明了什么？**
数据链路故障隔离：总线静默时前端保持最后有效值、WS 心跳不中断——与真车 ECU 掉线的表现一致，而不是整站崩溃。

**Q: CAN 帧怎么变成物理值？**
小端序拼接 data[0..1] → uint16 → 乘分辨率（0.1）→ 工程量值；车速 500Hz、水温 50Hz 分帧到达，各持最新值合成一条输出。
