#!/bin/bash
# 赛程控制验收:起模拟环境 -> 跳转/开赛/倍速/暂停 -> 读 GameStatus 断言
set -e
cd "$(dirname "$0")/.."
PY=.venv/bin/python
LOG=/tmp/rm_accept_server.log

cleanup() {
  pkill -f 'sim.match_server' 2>/dev/null || true
  pkill -f 'mosquitto -p 3333' 2>/dev/null || true
}
trap cleanup EXIT
cleanup
sleep 0.5

mosquitto -p 3333 >/dev/null 2>&1 &
disown
sleep 1
RM_HOLD=1 $PY -u -m sim.match_server >"$LOG" 2>&1 &
disown
sleep 1.5

echo "[1/4] 复位 -> 跳到第 100 秒 -> 开赛"
$PY sim/match_control.py reset >/dev/null
$PY sim/match_control.py seek 100 >/dev/null
$PY sim/match_control.py start >/dev/null
sleep 2

echo "[2/4] 切 5 倍速跑 2 秒"
$PY sim/match_control.py speed 5 >/dev/null
sleep 2

echo "[3/4] 暂停"
$PY sim/match_control.py pause >/dev/null

echo "[4/4] 从 GameStatus 读实时状态"
$PY - <<'EOF'
import sys, time
sys.path[:0] = ['.', 'generated']
import paho.mqtt.client as mqtt
import rm_terminal_pb2 as pb

def read():
    box = {}
    def on_msg(c, u, m):
        s = pb.GameStatus()
        s.ParseFromString(m.payload)
        box.update(stage=s.current_stage, elapsed=s.stage_elapsed_sec)
        c.disconnect()
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"accept-{time.time()}")
    c.on_message = on_msg
    c.connect("127.0.0.1", 3333)
    c.subscribe("GameStatus")
    c.loop_start()
    for _ in range(10):
        if box:
            break
        time.sleep(0.2)
    c.loop_stop()
    return box

a = read()
time.sleep(1.2)
b = read()
print("暂停瞬间:", a)
print("1.2 秒后:", b)
assert a and b, "没读到 GameStatus"
assert a["stage"] == 4, f"阶段应为 4(比赛中),实际 {a['stage']}"
assert 105 <= a["elapsed"] <= 140, f"赛程应落在 105~140s,实际 {a['elapsed']}"
assert abs(a["elapsed"] - b["elapsed"]) <= 1, "暂停后时钟仍在走"
print("PASS: 跳转、开赛、5 倍速、暂停全部生效")
EOF
