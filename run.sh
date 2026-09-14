#!/bin/bash
# 一键启动本地模拟环境 + 自定义终端
# 用法：./run.sh            正常
#      ./run.sh 0.02       注入 2% 图传丢包
set -e
cd "$(dirname "$0")"

PY=(../venv/bin/python -u)
LOSS="${1:-0}"
LOGS=/tmp/rm_terminal_logs
mkdir -p "$LOGS"

# 残留进程会占用 3333 并在 3334 上叠加第二个图传源，导致分片重组错乱
pkill -f 'venv/bin/python -u main.py' 2>/dev/null || true
pkill -f 'sim.match_server' 2>/dev/null || true
pkill -f 'sim.video_sender' 2>/dev/null || true
pkill -f 'mosquitto -p 3333' 2>/dev/null || true
sleep 1

cleanup() {
  echo ""
  echo "[run] 停止所有进程..."
  for p in $MOSQ $SERVER $VIDEO; do
    [ -n "$p" ] && kill "$p" 2>/dev/null || true
  done
  wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

echo "[run] 1/4 启动 MQTT Broker (模拟赛事引擎服务器，端口 3333)"
mosquitto -p 3333 >"$LOGS/mosquitto.log" 2>&1 &
MOSQ=$!
sleep 1

echo "[run] 2/4 启动模拟赛事服务器 + 机器人遥测"
"${PY[@]}" -m sim.match_server >"$LOGS/server.log" 2>&1 &
SERVER=$!
sleep 1

echo "[run] 3/4 启动模拟图传发送端 (丢包率 $LOSS)"
"${PY[@]}" -m sim.video_sender --loss "$LOSS" >"$LOGS/video.log" 2>&1 &
VIDEO=$!
sleep 1

echo "[run] 4/4 启动自定义终端 UI"
echo "[run] 日志目录: $LOGS"
echo ""
"${PY[@]}" main.py
