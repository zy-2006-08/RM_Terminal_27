#!/bin/bash
# 双击启动：mosquitto + 模拟器 + 图传 + 终端 + 点击式控制面板。关掉这个窗口即全部停止。
cd "$(dirname "$0")" || exit 1
.venv/bin/python sim/control_panel.py &
PANEL=$!
trap 'kill "$PANEL" 2>/dev/null' EXIT
./run.sh
