#!/usr/bin/env python3
# 简单的 TCP mock server，接收换行分隔的 JSON，打印出来
# 用法: python3 tests/mock_server.py [端口]
import socket
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9000

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('0.0.0.0', PORT))
srv.listen(1)
print(f"mock server listening on :{PORT}", flush=True)

while True:
    conn, addr = srv.accept()
    print(f"[+] client connected: {addr}", flush=True)
    buf = b''
    try:
        while True:
            data = conn.recv(4096)
            if not data:
                break
            buf += data
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                print(f"  {line.decode('utf-8')}", flush=True)
    except Exception as e:
        print(f"[!] error: {e}", flush=True)
    finally:
        conn.close()
        print(f"[-] client disconnected", flush=True)
