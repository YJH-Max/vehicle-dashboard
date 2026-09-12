#!/usr/bin/env bash
# 一键安装项目依赖。假设 Ubuntu 22.04+。
# 用法: ./scripts/install_deps.sh
set -e

echo "==> 安装编译必需依赖"
sudo apt update
sudo apt install -y \
    build-essential cmake git \
    libssl-dev zlib1g-dev

echo "==> 安装调试/测试依赖（可选，失败不阻塞）"
sudo apt install -y can-utils || echo "  [warn] can-utils 安装失败"
sudo apt install -y python3-websockets || echo "  [warn] python3-websockets 安装失败"
sudo apt install -y wrk || echo "  [warn] wrk 安装失败（Ubuntu 24.04 默认源无此包）"

echo "==> 下载 header-only 库"
cd "$(dirname "$0")/.."
mkdir -p include

if [ ! -f include/json.hpp ]; then
    echo "  - nlohmann/json"
    wget -q -O include/json.hpp \
        https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
fi

echo "==> 编译安装 uWebSockets"
if [ ! -f /usr/local/include/uWebSockets/App.h ] || [ ! -f /usr/lib/libuSockets.a ]; then
    TMP=$(mktemp -d)
    cd "$TMP"
    git clone --recurse-submodules https://github.com/uNetworking/uWebSockets.git
    cd uWebSockets
    make
    sudo make install
    cd uSockets
    ar rcs libuSockets.a *.o
    sudo cp libuSockets.a /usr/lib/
    sudo ldconfig
    cd -
    rm -rf "$TMP"
fi

echo "==> 验证"
ls /usr/local/include/uWebSockets/App.h
ls /usr/lib/libuSockets.a
ls include/json.hpp
echo "✅ 依赖就绪"
