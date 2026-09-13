# 基准测试报告

## 测试环境
- VMware 虚拟机 / Ubuntu 24.04 / 本地回环（非裸金属，数字偏保守）
- 后端: C++20 + uWebSockets（单线程事件循环）+ SPSC RingBuffer
- 数据流: 生产端 1000 Hz → RingBuffer → 20 Hz 定时广播
- 工具: wrk（HTTP）/ tests/ws_bench.py（WS，单进程 asyncio 客户端）

## HTTP `/api/history` — wrk -t2 -c100 -d15s
| 指标 | 结果 |
|---|---|
| QPS | **3410.75** |
| 延迟 P50 / P99 | 28.41ms / 52.23ms |
| 平均响应大小 | ~8.4 KB（27.72 MB/s ÷ QPS） |
| 错误 | 0（51356 请求） |

## WebSocket `/ws` — tests/ws_bench.py
| 并发 | 时长 | 吞吐 msg/s | avg | P50 | P99 | 断连/丢包 |
|---|---|---|---|---|---|---|
| 50  | 10s | 990  | 7.5ms   | 6.6ms   | 30.6ms   | 0 / 0 |
| 100 | 10s | 1970 | 13.7ms  | 12.8ms  | 43.1ms   | 0 / 0 |
| 200 | 10s | 3898 | 27.9ms  | 26.7ms  | 55.2ms   | 0 / 0 |
| 500 | 10s | 9484 | 260.9ms | 190.8ms | 1083.5ms | 0 / 0 |
| 500 | 30s | 9858 | 168.0ms | 159.8ms | 377.3ms  | 0 / 0 |

## 瓶颈分析
- 吞吐随并发**近线性**：各档均达理论值（20Hz × 连接数）的 94% 以上
- 200 并发内 P99 < 60ms，延迟随并发线性增长——单线程事件循环的固有成本
- 500 并发（≈10k msg/s）为当前配置的**发送饱和点**：消息进入稳定排队，
  30s 长跑证实延迟收敛（P99 1083ms→377ms）、队列不发散、无连接异常
- WS 延迟含客户端（单进程 Python asyncio）自身开销，为保守估计

## 架构对比：HTTP 轮询 vs WebSocket 推送
| 维度 | HTTP 轮询 | WS 推送 | 改善 |
|---|---|---|---|
| 单客户端带宽 @20Hz | ~168 KB/s（8.4KB×20） | ~2 KB/s（~100B×20） | **≈84×** |
| 服务端序列化次数 | 每请求一次，O(客户端数) | 每 tick 一次，O(1) | 与客户端数解耦 |
| 连接模型 | 高频短请求 | 单条长连接 | 500 并发实测验证 |

> 结论：等价刷新率下每客户端传输开销降低约两个数量级，
> 服务端负载从 O(请求量) 转移为 O(时钟节拍)，并消除轮询空转。

## SPSC → MPMC 队列演进（同机对比，Release）

测试环境：VMware / Ubuntu 24.04 / g++ 13.3 / **2 vCPU（nproc=2）**

| 队列类型 | 拓扑 | 吞吐 | 说明 |
|---------|------|------|------|
| SPSC RingBuffer | 1P1C | 7.46 M items/sec | 基线，不支持多生产者 |
| MPMC (Vyukov) | 1P1C | 5.27 M items/sec | CAS 引入约 29% 单线程开销 |
| MPMC (Vyukov) | 4P4C | **10.88 M items/sec** | `pushed == consumed == 8,000,000`，耗时 0.74s |

要点：

- 2 vCPU 环境下 8 线程并行，MPMC 4P4C 相对 1P1C 提升 106%（5.27 → 10.88）
- 相对 SPSC 单线程基线提升 46%（7.46 → 10.88）
- 单线程回退是预期成本：CAS 比纯 load/store 贵，但换来了多核扩展性
- 全链路 CAN 模式（--can vcan0）保持 ~550 条/秒，数据源 / 前端零改动

## MPMC 正确性验证

- 4 生产者 × 4 消费者 × 200 万条 = 800 万条
- pushed == consumed == expected == 8000000
- 结果：PASS（无丢失、无重复、无死锁）

## 阶段 2：TopicBus 多订阅者验证

- 数据源：CAN vcan0 550 条/秒 稳定写入 TopicBus
- 订阅者：ws_consumer (20Hz 合并推送) + alarm_consumer (实时阈值判断)
- 修复：2 核 VM 上 4 个忙等线程饥饿导致 uWS 主线程无法接受新连接
- 现状：1ms sleep 止血；后续将升级为 condition_variable 阻塞唤醒

## MutexQueue 对照实验

同条件（2 vCPU / 每生产者 200 万条 / 队列容量 4096）对比 `mutex + deque` 与 Vyukov MPMC。

| 拓扑 | Mutex+deque | MPMC (Vyukov) | 相对差异 |
|------|-------------|---------------|---------|
| 1P1C | 6.71 M/s | **9.52 M/s** | MPMC 快 42% |
| 4P4C | 13.56 M/s | 13.57 M/s | 基本持平 |

**1P1C**：单线程路径无竞争时，`mutex` 的 lock/unlock 开销明显高于纯原子 load/store。这与预期一致。

**4P4C 打平，出乎预期。原因分析**：

1. **2 vCPU 是主因**：8 线程挤 2 核，主要成本是调度切换，而非锁竞争或 CAS 竞争。CPU 时间片把差异摊平。
2. **glibc mutex 有自适应自旋**：短临界区（deque 的 push/pop）先自旋再挂起，实际开销比"典型教科书"描述的小。
3. **MPMC 的 CAS 在 4 生产者间也互相失败重试**：等于一个用户态自旋锁，与 mutex 成本接近。

**结论**：本项目 550 帧/秒的负载，`mutex + deque` 完全够用。选择 MPMC 的理由**不是当前性能**，而是：

- 单线程路径上明确更快（42%），为数据通道的性能余量预留
- 拓扑上支持多生产者——未来多 ECU 汇聚不需要改架构
- 有明确的量化证据支撑这个取舍，而不是"无锁一定更快"的教条

**这件事的工程价值**：先假设、再测量、发现结果与预期不符、分析原因、把"意外"写进文档。这是工程判断，不是背结论。


## ThreadSanitizer 检测

用 -fsanitize=thread -g -O1 重新编译并运行单元测试与 MPMC 基准。

    mkdir -p build-tsan && cd build-tsan
    cmake -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" \
          -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" ..
    cmake --build . -j$(nproc) --target test_ring test_topic_bus test_data_pool bench_mpmc

    # 内核 ASLR 熵值与 TSan shadow memory 布局不兼容时，需禁用地址随机化
    setarch $(uname -m) -R ./test_ring
    setarch $(uname -m) -R ./test_topic_bus
    setarch $(uname -m) -R ./test_data_pool
    setarch $(uname -m) -R ./bench_mpmc

| 测试 | 结果 |
|------|------|
| test_ring（SPSC/MPMC 并发正确性） | 0 race |
| test_topic_bus（多订阅者分发与句柄生命周期） | 0 race |
| bench_mpmc（4P4C × 800 万条，pushed == consumed） | 0 race |
| test_data_pool（降采样与容量边界） | 0 race |

结论：acquire/release 内存序配对、alignas(64) 缓存行隔离、deliver 锁内快照-锁外回调，均未见数据竞争报告。

环境说明：

- 内核 7.0.0-28-generic 的 ASLR 熵值与 TSan 影子映射不兼容，直接运行会报 FATAL: ThreadSanitizer: unexpected memory mapping，需用 setarch -R 禁用地址随机化。这是 TSan 与内核版本的已知兼容性问题，不是代码问题。
- TSan 插桩后 bench_mpmc 从 10.88 M/s 降到 1.45 M/s（约 7.5 倍开销），这是每次内存访问都查影子内存的固有代价，不是性能回归。
