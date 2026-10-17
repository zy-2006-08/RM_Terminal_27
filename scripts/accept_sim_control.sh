#!/bin/bash
# 赛程控制验收:起模拟环境 -> 跳转/开赛/倍速/暂停 -> 读 GameStatus 断言
set -e
cd "$(dirname "$0")/.."
PY=.venv/bin/python
LOG=/tmp/rm_accept_server.log

cleanup() {
  pkill -f 'sim.match_server' 2>/dev/null || true
  pkill -f 'mosquitto -p 3333' 2>/dev/null || true
  # 控制面板会自己重连到任意 3333 broker,留在后台会往验收环境里发指令(曾把
  # 断言里的 123 血覆盖成 200),所以和服务器一起清掉。
  pkill -f 'sim/control_panel.py' 2>/dev/null || true
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

echo "[1/6] 复位 -> 跳到第 100 秒 -> 开赛"
$PY sim/match_control.py reset >/dev/null
$PY sim/match_control.py seek 100 >/dev/null
$PY sim/match_control.py start >/dev/null
sleep 2

echo "[2/6] 切 5 倍速跑 2 秒"
$PY sim/match_control.py speed 5 >/dev/null
sleep 2

echo "[3/6] 暂停"
$PY sim/match_control.py pause >/dev/null

echo "[4/6] 从 GameStatus 读实时状态"
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

echo "[5/6] 关随机掉血,把蓝1重装设为 123 血"
$PY sim/match_control.py auto 0 >/dev/null
$PY sim/match_control.py hp 101 123 >/dev/null
sleep 0.6
$PY - <<'EOF'
import sys, time
sys.path[:0] = ['.', 'generated']
import paho.mqtt.client as mqtt
import rm_terminal_pb2 as pb

def read_hp(robot_id):
    box = {}
    def on_msg(c, u, m):
        s = pb.RobotHealthSet()
        s.ParseFromString(m.payload)
        for e in s.entries:
            if e.robot_id == robot_id:
                box["hp"] = e.current_hp
        c.disconnect()
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"accept-hp-{time.time()}")
    c.on_message = on_msg
    c.connect("127.0.0.1", 3333)
    c.subscribe("RobotHealthSet")
    c.loop_start()
    for _ in range(10):
        if box:
            break
        time.sleep(0.2)
    c.loop_stop()
    return box

a = read_hp(101)
time.sleep(1.2)
b = read_hp(101)
print("蓝1重装:", a, "-> 1.2 秒后:", b)
assert a.get("hp") == 123, f"血量应为 123,实际 {a}"
assert b.get("hp") == 123, "关掉随机掉血后血量仍被覆盖"
print("PASS: 机器人血量可手动设置且不被随机覆盖")
EOF

echo "[6/6] 设置阵地并验证控制度增长"
$PY sim/match_control.py start >/dev/null
$PY sim/match_control.py speed 5 >/dev/null
$PY sim/match_control.py zone 0 2 >/dev/null   # 红方前哨:红方完全控制 +1/秒
$PY sim/match_control.py zone 3 4 >/dev/null   # 蓝方基地:蓝方完全控制 +3/秒
sleep 1
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
        box.update(red_outpost_zone=s.red_outpost_zone,
                   blue_outpost_zone=s.blue_outpost_zone,
                   red_base_zone=s.red_base_zone,
                   blue_base_zone=s.blue_base_zone,
                   red_cp=s.red_control_points, blue_cp=s.blue_control_points,
                   red_rf=s.red_reinforce_chances, blue_rf=s.blue_reinforce_chances)
        c.disconnect()
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"accept-zone-{time.time()}")
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
time.sleep(0.8)
b = read()
print("阵地:", {k: a[k] for k in a if k.endswith("_zone")})
print("控制度:", a["red_cp"], a["blue_cp"], "->", b["red_cp"], b["blue_cp"])
assert a and b, "没读到 GameStatus"
assert a["red_outpost_zone"] == 2, f"红前哨应为 2,实际 {a['red_outpost_zone']}"
assert a["blue_outpost_zone"] == 0, f"蓝前哨应保持未控 0,实际 {a['blue_outpost_zone']}"
assert a["red_base_zone"] == 0, f"红基地应保持未控 0,实际 {a['red_base_zone']}"
assert a["blue_base_zone"] == 4, f"蓝基地应为 4,实际 {a['blue_base_zone']}"
assert b["red_cp"] > a["red_cp"], f"红方控制度应增长,实际 {a['red_cp']} -> {b['red_cp']}"
assert b["blue_cp"] > a["blue_cp"], f"蓝方控制度应增长,实际 {a['blue_cp']} -> {b['blue_cp']}"
assert (b["blue_cp"] - a["blue_cp"]) > (b["red_cp"] - a["red_cp"]), "基地 3/秒 应快于前哨 1/秒"
assert a["red_rf"] == a["blue_rf"], "加固机会应双方同步累积"
print("PASS: 阵地状态可设置,控制度按阵地类型累计")
EOF
