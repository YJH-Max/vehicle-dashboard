#!/usr/bin/env bash
set -e
sudo modprobe vcan
ip link show vcan0 &>/dev/null || sudo ip link add dev vcan0 type vcan
sudo ip link set vcan0 up
echo "✅ vcan0 已就绪"
