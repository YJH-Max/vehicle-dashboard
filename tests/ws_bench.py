#!/usr/bin/env python3
# 用法: python3 tests/ws_bench.py [并发连接数] [持续秒数]
import asyncio, json, sys, time
try:
    import websockets
except ImportError:
    sys.exit("缺依赖: sudo apt install python3-websockets")

URL = "ws://localhost:8080/ws"
TS_KEYS = ("timestamp", "ts", "server_ts", "time_ms", "t")

def get_ts(data):
    """从消息里找毫秒级 epoch 时间戳，兼容秒级"""
    if isinstance(data, dict):
        for k in TS_KEYS:
            v = data.get(k)
            if isinstance(v, (int, float)):
                if v > 1_000_000_000_000:   # 毫秒级
                    return v
                if v > 1_000_000_000:       # 秒级
                    return v * 1000
    return None

async def worker(stats, run):
    try:
        async with websockets.connect(URL, open_timeout=5) as ws:
            stats["ok"] += 1
            while run.is_set():
                msg = await asyncio.wait_for(ws.recv(), timeout=5)
                stats["msgs"] += 1
                if not stats["sample"]:
                    stats["sample"] = msg[:200]
                ts = get_ts(json.loads(msg))
                if ts:
                    stats["lat"].append(time.time() * 1000 - ts)
    except asyncio.CancelledError:
        pass
    except Exception:
        stats["fail"] += 1

async def main():
    n   = int(sys.argv[1]) if len(sys.argv) > 1 else 50
    dur = int(sys.argv[2]) if len(sys.argv) > 2 else 10
    stats = {"ok": 0, "fail": 0, "msgs": 0, "lat": [], "sample": ""}
    run = asyncio.Event(); run.set()
    tasks = [asyncio.create_task(worker(stats, run)) for _ in range(n)]
    await asyncio.sleep(dur)
    run.clear()
    for t in tasks: t.cancel()
    await asyncio.gather(*tasks, return_exceptions=True)

    print(f"—— WS 压测: {n} 并发 × {dur}s ——")
    print(f"连接成功: {stats['ok']}   失败: {stats['fail']}")
    print(f"吞吐: {stats['msgs']/dur:.0f} msg/s")
    print(f"消息样例: {stats['sample']}")
    lat = sorted(stats["lat"])
    if lat:
        print(f"延迟 avg={sum(lat)/len(lat):.1f}ms  "
              f"P50={lat[len(lat)//2]:.1f}ms  "
              f"P99={lat[min(int(len(lat)*.99), len(lat)-1)]:.1f}ms")
    else:
        print("延迟: N/A（没找到时间戳字段，看上面消息样例）")

asyncio.run(main())
