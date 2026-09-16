#!/bin/bash
# 一键启动本地模拟环境 + 原生 C++ 终端
# 用法：./run.sh                      正常
#      ./run.sh 0.02                 注入 2% 图传丢包
#      BUILD_DIR=build/release ./run.sh
set -e
cd "$(dirname "$0")"

# 模拟器仍是 Python：sim/ 是终端唯一的数据源，也是 mode_regression_e2e 的依赖
PY=(../venv/bin/python -u)
BUILD_DIR="${BUILD_DIR:-build/macos}"
TERMINAL_BIN="$BUILD_DIR/rm_terminal"
LOSS="${1:-0}"
LOGS=/tmp/rm_terminal_logs
mkdir -p "$LOGS"

if [ ! -x "$TERMINAL_BIN" ]; then
  echo "[run] 找不到可执行文件 $TERMINAL_BIN"
  echo "[run] 先构建：cmake -S . -B $BUILD_DIR && cmake --build $BUILD_DIR"
  exit 1
fi

# 残留进程会占用 3333 并在 3334 上叠加第二个图传源，导致分片重组错乱。
# 模式不能写成 'sim.match_server'：从 sim/ 目录里直接 `-m match_server` 起的进程
# 不匹配该模式，会作为第二个 GameStatus 发布者存活下来，用自己的 RM_MATCH_SEC
# 覆盖倒计时（曾导致终端一连接就只剩十几秒并立刻判胜）。
pkill -f 'rm_terminal' 2>/dev/null || true
pkill -f 'match_server' 2>/dev/null || true
pkill -f 'video_sender' 2>/dev/null || true
pkill -f 'mosquitto -p 3333' 2>/dev/null || true
sleep 1

for _ in 1 2 3; do
  STRAY="$(pgrep -f 'match_server|video_sender' || true)"
  [ -z "$STRAY" ] && break
  echo "[run] 等待残留发布者退出: $STRAY"
  kill $STRAY 2>/dev/null || true
  sleep 1
done
STRAY="$(pgrep -f 'match_server|video_sender' || true)"
if [ -n "$STRAY" ]; then
  echo "[run] 残留发布者无法清理，请手动处理: $STRAY"
  exit 1
fi

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

echo "[run] 4/4 启动原生终端 UI ($TERMINAL_BIN)"
echo "[run] 日志目录: $LOGS"
echo ""
"$TERMINAL_BIN"
