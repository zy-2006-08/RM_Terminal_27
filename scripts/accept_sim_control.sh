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
  sleep 0.3
  # 旧服务器残留时会和新服务器抢同一个 MQTT client_id,两边状态来回覆盖。
  pkill -9 -f 'sim.match_server' 2>/dev/null || true
  pkill -9 -f 'mosquitto -p 3333' 2>/dev/null || true
  pkill -9 -f 'sim/control_panel.py' 2>/dev/null || true
}
trap cleanup EXIT
cleanup
if pgrep -f 'sim.match_server' >/dev/null; then
  echo "残留模拟器进程无法清理:" >&2
  pgrep -fl 'sim.match_server' >&2
  exit 1
fi
sleep 0.5

mosquitto -p 3333 >/dev/null 2>&1 &
disown
sleep 1
RM_HOLD=1 RM_PREP_SEC=1 RM_CHECK_SEC=1 RM_COUNTDOWN_SEC=1 $PY -u -m sim.match_server >"$LOG" 2>&1 &
disown
sleep 1.5

# 面板崩在 import 阶段时 py_compile 查不出来,只能真启动看一眼。
echo "[panel] 控制面板启动自检"
$PY sim/control_panel.py >/tmp/rm_accept_panel.log 2>&1 &
PANEL=$!
sleep 2
if ! kill -0 "$PANEL" 2>/dev/null; then
  cat /tmp/rm_accept_panel.log
  echo "control panel exited during startup" >&2
  exit 1
fi
kill "$PANEL" 2>/dev/null || true

echo "[1/8] 复位 -> 跳到第 100 秒 -> 开赛"
$PY sim/match_control.py reset >/dev/null
$PY sim/match_control.py seek 100 >/dev/null
$PY sim/match_control.py start >/dev/null
sleep 2

echo "[2/8] 切 5 倍速跑 2 秒"
$PY sim/match_control.py speed 5 >/dev/null
sleep 2

echo "[3/8] 暂停"
$PY sim/match_control.py pause >/dev/null

echo "[4/8] 从 GameStatus 读实时状态"
$PY - <<'EOF'
import sys, time
sys.path[:0] = ['.', 'generated']
import paho.mqtt.client as mqtt
import rm_terminal_pb2 as pb

def read():
    """收 0.5s 内的帧取最新:指令和状态上报都是异步的,首帧可能是指令生效前的。"""
    box = {}
    def on_msg(c, u, m):
        s = pb.GameStatus()
        s.ParseFromString(m.payload)
        box.update(stage=s.current_stage, elapsed=s.stage_elapsed_sec)
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"accept-{time.time()}")
    c.on_message = on_msg
    c.connect("127.0.0.1", 3333)
    c.subscribe("GameStatus")
    c.loop_start()
    for _ in range(10):
        if box:
            break
        time.sleep(0.2)
    time.sleep(0.5)
    c.disconnect()
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

echo "[5/8] 关随机掉血,把蓝1重装设为 123 血"
$PY sim/match_control.py auto 0 >/dev/null
$PY sim/match_control.py hp 101 123 >/dev/null
sleep 0.6
$PY - <<'EOF'
import sys, time
sys.path[:0] = ['.', 'generated']
import paho.mqtt.client as mqtt
import rm_terminal_pb2 as pb

def read_hp(robot_id, duration=1.2):
    """整段窗口内的血量都收下来:同时有旧进程在发时,单帧读取会碰巧通过。"""
    values = []
    def on_msg(c, u, m):
        s = pb.RobotHealthSet()
        s.ParseFromString(m.payload)
        for e in s.entries:
            if e.robot_id == robot_id:
                values.append(e.current_hp)
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"accept-hp-{time.time()}")
    c.on_message = on_msg
    c.connect("127.0.0.1", 3333)
    c.subscribe("RobotHealthSet")
    c.loop_start()
    time.sleep(duration)
    c.disconnect()
    c.loop_stop()
    return values

a = read_hp(101)
time.sleep(0.5)
b = read_hp(101)
print("蓝1重装:", a, "-> 0.5 秒后:", b)
assert a and all(v == 123 for v in a), f"血量应持续为 123,实际 {a}"
assert b and all(v == 123 for v in b), f"关掉随机掉血后血量仍被覆盖: {b}"
print("PASS: 机器人血量可手动设置且不被随机覆盖")
EOF

echo "[6/8] 设置阵地并验证控制度增长"
$PY sim/match_control.py start >/dev/null
$PY sim/match_control.py speed 5 >/dev/null
$PY sim/match_control.py zone 0 2 >/dev/null   # 红方前哨:红方完全控制
$PY sim/match_control.py zone 3 4 >/dev/null   # 蓝方基地:蓝方完全控制
sleep 1
$PY sim/match_control.py pause >/dev/null
$PY - <<'EOF'
import sys, time, subprocess
sys.path[:0] = ['.', 'generated']
import paho.mqtt.client as mqtt
import rm_terminal_pb2 as pb

def read():
    """收 0.5s 内的帧取最新:指令异步生效,首帧可能是旧状态。"""
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
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"accept-zone-{time.time()}")
    c.on_message = on_msg
    c.connect("127.0.0.1", 3333)
    c.subscribe("GameStatus")
    c.loop_start()
    for _ in range(10):
        if box:
            break
        time.sleep(0.2)
    time.sleep(0.5)
    c.disconnect()
    c.loop_stop()
    return box

subprocess.run([sys.executable, "sim/match_control.py", "zone", "1", "1"], check=True,
               stdout=subprocess.DEVNULL)
t = read()
assert t and t["blue_outpost_zone"] == 1, f"蓝前哨应先为红方临时控制 1,实际 {t}"
subprocess.run([sys.executable, "sim/match_control.py", "speed", "1"], check=True,
               stdout=subprocess.DEVNULL)
subprocess.run([sys.executable, "sim/match_control.py", "start"], check=True,
               stdout=subprocess.DEVNULL)
subprocess.run([sys.executable, "sim/match_control.py", "hold", "1", "1", "6"], check=True,
               stdout=subprocess.DEVNULL)
subprocess.run([sys.executable, "sim/match_control.py", "pause"], check=True,
               stdout=subprocess.DEVNULL)
p = read()
assert p and p["blue_outpost_zone"] == 1, f"持续激活只维持临时控制,应保持 1,实际 {p}"
subprocess.run([sys.executable, "sim/match_control.py", "speed", "5"], check=True,
               stdout=subprocess.DEVNULL)
subprocess.run([sys.executable, "sim/match_control.py", "zone", "2", "3"], check=True,
               stdout=subprocess.DEVNULL)
a = read()
assert a and a["red_base_zone"] == 3, f"红基地应为蓝方临时控制 3,实际 {a}"
subprocess.run([sys.executable, "sim/match_control.py", "start"], check=True,
               stdout=subprocess.DEVNULL)
time.sleep(1.5)
b = read()
print("阵地:", {k: a[k] for k in a if k.endswith("_zone")})
print("控制度:", a["red_cp"], a["blue_cp"], "->", b["red_cp"], b["blue_cp"])
assert b, "没读到 GameStatus"
assert a["red_outpost_zone"] == 2, f"红前哨应为 2,实际 {a['red_outpost_zone']}"
assert a["blue_outpost_zone"] == 1, f"蓝前哨应保持红方临时控制 1,实际 {a['blue_outpost_zone']}"
assert b["red_base_zone"] == 2, f"临时控制 5 秒未激活应回退为红方完全控制 2,实际 {b['red_base_zone']}"
assert a["blue_base_zone"] == 4, f"蓝基地应为 4,实际 {a['blue_base_zone']}"
assert b["red_cp"] > a["red_cp"], f"红方控制度应增长,实际 {a['red_cp']} -> {b['red_cp']}"
assert b["blue_cp"] > a["blue_cp"], f"蓝方控制度应增长,实际 {a['blue_cp']} -> {b['blue_cp']}"
assert a["red_rf"] == a["blue_rf"], "加固机会应双方同步累积"
print("PASS: 阵地状态可设置,控制度按阵地类型累计")
EOF

echo "[7/8] 建筑血量/堡垒/致盲/加固机会"
# 加固机会按比赛秒累积(规则 5.2),seek 不补累计器,所以先倍速真跑一段。
# 随机掉血只在这段开启,用来把累计命中数推过 10 发门槛。
$PY sim/match_control.py speed 10 >/dev/null
$PY sim/match_control.py auto 1 >/dev/null
sleep 6
$PY sim/match_control.py speed 1 >/dev/null
$PY sim/match_control.py auto 0 >/dev/null
# 暂停后比赛时钟冻结,加固机会/致盲剩余时间都不会在断言期间漂移。
$PY sim/match_control.py pause >/dev/null
$PY sim/match_control.py structure 0 1234 >/dev/null   # 红方基地
$PY sim/match_control.py structure 2 777 >/dev/null    # 红方前哨
$PY sim/match_control.py fortress 1 >/dev/null         # 堡垒归红方
$PY sim/match_control.py blind 2 4 >/dev/null          # 蓝方中「基地随机移动目标」
sleep 0.8
$PY - <<'EOF'
import sys, time
sys.path[:0] = ['.', 'generated']
import paho.mqtt.client as mqtt
import rm_terminal_pb2 as pb

def read_both():
    game, blind = {}, {}
    def on_msg(c, u, m):
        if m.topic == "GameStatus":
            s = pb.GameStatus(); s.ParseFromString(m.payload)
            game.update(red_base_hp=s.red_base_hp, red_outpost_hp=s.red_outpost_hp,
                        blue_base_hp=s.blue_base_hp, fortress=s.fortress_holder,
                        red_rf=s.red_reinforce_chances, blue_rf=s.blue_reinforce_chances)
        elif m.topic == "BlindStatus":
            s = pb.BlindStatus(); s.ParseFromString(m.payload)
            blind.update(side=s.affected_faction, target=s.target_type,
                         self_blinded=s.self_base_blinded,
                         remain=s.blind_remaining_ms)
        if game and blind:
            c.disconnect()
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"accept-rule-{time.time()}")
    c.on_message = on_msg
    c.connect("127.0.0.1", 3333)
    c.subscribe([("GameStatus", 0), ("BlindStatus", 0)])
    c.loop_start()
    for _ in range(15):
        if game and blind:
            break
        time.sleep(0.2)
    c.loop_stop()
    return game, blind

game, blind = read_both()
print("建筑/堡垒:", game)
print("致盲:", blind)
assert game.get("red_base_hp") == 1234, f"红基地血量应为 1234,实际 {game}"
assert game.get("red_outpost_hp") == 777, f"红前哨血量应为 777,实际 {game}"
assert game.get("fortress") == 1, f"堡垒应归红方(1),实际 {game.get('fortress')}"
assert blind.get("side") == 2, f"致盲方应为蓝方(2),实际 {blind}"
assert blind.get("target") == 4, f"飞镖类型应为 4,实际 {blind}"
assert blind.get("self_blinded") is False, "红方视角不应显示我方基地致盲"
print("PASS: 建筑血量、堡垒归属、蓝方随机移动目标致盲")
EOF

$PY sim/match_control.py blind-clear >/dev/null
sleep 0.8
$PY - <<'EOF'
import sys, time
sys.path[:0] = ['.', 'generated']
import paho.mqtt.client as mqtt
import rm_terminal_pb2 as pb

def read_blind():
    box = {}
    def on_msg(c, u, m):
        s = pb.BlindStatus(); s.ParseFromString(m.payload)
        box.update(side=s.affected_faction, remain=s.blind_remaining_ms)
        c.disconnect()
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"accept-clear-{time.time()}")
    c.on_message = on_msg
    c.connect("127.0.0.1", 3333)
    c.subscribe("BlindStatus")
    c.loop_start()
    for _ in range(10):
        if box:
            break
        time.sleep(0.2)
    c.loop_stop()
    return box

b = read_blind()
print("解除后:", b)
assert b.get("side") == 0 and b.get("remain") == 0, f"手动致盲未解除: {b}"
print("PASS: 手动致盲可解除")
EOF

# 仍处于暂停,读数和命令之间加固机会不会自己增长。
$PY - <<'EOF'
import sys, time
sys.path[:0] = ['.', 'generated']
import paho.mqtt.client as mqtt
import rm_terminal_pb2 as pb

def read_rf():
    box = {}
    def on_msg(c, u, m):
        s = pb.GameStatus(); s.ParseFromString(m.payload)
        box.update(red=s.red_reinforce_chances, blue=s.blue_reinforce_chances)
        c.disconnect()
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"accept-rf-{time.time()}")
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

a = read_rf()
assert a, "没读到 GameStatus"
assert a["red"] > 0, f"红方加固机会应已累积,实际 {a}"
open("/tmp/rm_accept_rf_before", "w").write(str(a["red"]))
print("消耗前:", a)
EOF

$PY sim/match_control.py reinforce 1 >/dev/null
sleep 0.8
$PY - <<'EOF'
import sys, time
sys.path[:0] = ['.', 'generated']
import paho.mqtt.client as mqtt
import rm_terminal_pb2 as pb

before = int(open("/tmp/rm_accept_rf_before").read())

def read_rf():
    box = {}
    def on_msg(c, u, m):
        s = pb.GameStatus(); s.ParseFromString(m.payload)
        box.update(red=s.red_reinforce_chances, blue=s.blue_reinforce_chances)
        c.disconnect()
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"accept-rf2-{time.time()}")
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

b = read_rf()
print("消耗后:", b)
assert b["red"] == before - 1, f"红方加固机会应减 1:{before} -> {b['red']}"
print("PASS: 累计命中满 10 发后可消耗加固机会")
EOF

echo "[8/8] 飞镖开闸/发射"
$PY sim/match_control.py zone 1 4 >/dev/null
$PY sim/match_control.py seek 34 >/dev/null   # 比赛内 30s,第一个开闸窗口
$PY sim/match_control.py dart-open 1 >/dev/null
$PY sim/match_control.py seek 41 >/dev/null   # 比赛内 37s,7 秒开闸完成
$PY sim/match_control.py dart 1 1 >/dev/null
sleep 0.8
$PY - <<'EOF'
import sys, time, subprocess
sys.path[:0] = ['.', 'generated']
import paho.mqtt.client as mqtt
import rm_terminal_pb2 as pb

def read():
    box = {}
    def on_msg(c, u, m):
        s = pb.GameStatus(); s.ParseFromString(m.payload)
        box.update(blue_outpost=s.blue_outpost_hp, stage=s.current_stage,
                   blue_outpost_zone=s.blue_outpost_zone)
        c.disconnect()
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"accept-dart-{time.time()}")
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
print("飞镖后:", a)
assert a.get("blue_outpost") == 0, f"击毁后前哨站应倒下,实际 {a}"
assert a.get("blue_outpost_zone") == 3, f"完全控制被击毁后应降为蓝方临时控制,实际 {a}"
for second in range(42, 47):
    subprocess.run([sys.executable, "sim/match_control.py", "seek", str(second)],
                   check=True, stdout=subprocess.DEVNULL)
    time.sleep(0.3)
b = read()
print("5 秒未激活后:", b)
assert b.get("blue_outpost") == 500, f"5s 未激活后应重建 500 HP,实际 {b}"
assert b.get("blue_outpost_zone") == 2, f"5s 未激活后应转红方完全控制,实际 {b}"
print("PASS: 开闸窗口内发射飞镖,击毁后按表 5-21 转化并重建")
EOF
