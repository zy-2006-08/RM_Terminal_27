#!/bin/bash
# 模拟器验收:起模拟环境 -> 赛程控制、血量、阵地、建筑、堡垒、飞镖逐项操作 ->
# 读模拟器发给终端的 2027 官方消息断言(席位固定为红方 3 号步兵,「己方」= 红方)。
set -e
cd "$(dirname "$0")/.."
PY=.venv/bin/python
LOG=/tmp/rm_accept_server.log
export RM_SELF_ID=3

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
  rm -f /tmp/rm_accept_probe.py
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

# 各段断言共用的读取函数:收一段时间内的帧取最新的一帧(指令和状态上报都是异步的,
# 首帧可能还是指令生效前的),Event 则全部收下。
cat > /tmp/rm_accept_probe.py <<'EOF'
import subprocess, sys, time
sys.path[:0] = ['.', 'generated']
import paho.mqtt.client as mqtt
import rm_terminal_pb2 as pb

MESSAGES = {"GameStatus": pb.GameStatus, "GlobalUnitStatus": pb.GlobalUnitStatus,
            "GlobalSpecialMechanism": pb.GlobalSpecialMechanism, "Event": pb.Event}


def control(*args):
    subprocess.run([sys.executable, "sim/match_control.py", *map(str, args)], check=True,
                   stdout=subprocess.DEVNULL)


def read(topics, duration=1.3, during=None):
    """订阅 topics,可选在订阅后执行 during();返回 {topic: 最新一帧},Event 为全部列表。"""
    latest, events = {}, []
    def on_msg(c, u, m):
        message = MESSAGES[m.topic]()
        message.ParseFromString(m.payload)
        if m.topic == "Event":
            events.append((message.event_id, message.param))
        else:
            latest[m.topic] = message
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"accept-{time.time()}")
    c.on_message = on_msg
    c.connect("127.0.0.1", 3333)
    c.subscribe([(topic, 0) for topic in topics])
    c.loop_start()
    time.sleep(0.3)
    if during:
        during()
    time.sleep(duration)
    c.disconnect()
    c.loop_stop()
    latest["Event"] = events
    return latest
EOF

echo "[1/7] 复位 -> 跳到第 100 秒 -> 开赛 -> 5 倍速 -> 暂停"
$PY sim/match_control.py reset >/dev/null
$PY sim/match_control.py seek 100 >/dev/null
$PY sim/match_control.py start >/dev/null
sleep 2
$PY sim/match_control.py speed 5 >/dev/null
sleep 2
$PY sim/match_control.py pause >/dev/null
$PY - <<'EOF'
exec(open("/tmp/rm_accept_probe.py").read())
a = read(["GameStatus"])["GameStatus"]
time.sleep(1.2)
b = read(["GameStatus"])["GameStatus"]
print("暂停瞬间:", a.current_stage, a.stage_elapsed_sec, "1.2 秒后:", b.stage_elapsed_sec)
assert a.current_stage == 4, f"阶段应为 4(比赛中),实际 {a.current_stage}"
assert 105 <= a.stage_elapsed_sec <= 140, f"赛程应落在 105~140s,实际 {a.stage_elapsed_sec}"
assert abs(a.stage_elapsed_sec - b.stage_elapsed_sec) <= 1, "暂停后时钟仍在走"
assert a.is_paused, "比赛中暂停要报 is_paused"
assert a.game_result == 255 and a.end_reason == 255, "非结算阶段胜负和原因都报 255"
print("PASS: 跳转、开赛、5 倍速、暂停全部生效")
EOF

echo "[2/7] 关随机掉血,把蓝 1 重装设为 123 血"
$PY sim/match_control.py auto 0 >/dev/null
$PY sim/match_control.py hp 101 123 >/dev/null
sleep 0.6
$PY - <<'EOF'
exec(open("/tmp/rm_accept_probe.py").read())
# 红方席位的 robot_health:己方 1,3,4,7 在前,对方 101,103,104,107 在后,蓝 1 是第 5 个。
a = read(["GlobalUnitStatus"], 2.2)["GlobalUnitStatus"]
time.sleep(0.5)
b = read(["GlobalUnitStatus"], 2.2)["GlobalUnitStatus"]
print("robot_health:", list(a.robot_health), "->", list(b.robot_health))
assert len(a.robot_health) == 8 and len(a.robot_bullets) == 4, "8 个血量、4 个己方发弹量"
assert a.robot_health[4] == 123 and b.robot_health[4] == 123, "蓝 1 重装应保持 123"
print("PASS: 机器人血量可手动设置,按官方顺序上报且不被随机覆盖")
EOF

echo "[3/7] 设置阵地并验证控制度增长"
$PY sim/match_control.py start >/dev/null
$PY sim/match_control.py speed 5 >/dev/null
$PY sim/match_control.py zone 0 2 >/dev/null   # R 前哨阵地:红方完全控制
$PY sim/match_control.py zone 3 4 >/dev/null   # B 基地阵地:蓝方完全控制
sleep 1
$PY sim/match_control.py pause >/dev/null
$PY - <<'EOF'
exec(open("/tmp/rm_accept_probe.py").read())
t = read(["GlobalUnitStatus", "Event"], 1.3, lambda: control("zone", 1, 1))
assert t["GlobalUnitStatus"].outpost_B_status == 2, \
    f"B 前哨阵地被红方临时控制,对红方席位是「己方临时」2,实际 {t['GlobalUnitStatus'].outpost_B_status}"
assert (6, "1") in t["Event"], f"阵地易手要发 Event 6 参数 1(红方临时),实际 {t['Event']}"
control("speed", 1)
control("start")
control("hold", 1, 1, 6)
control("pause")
p = read(["GlobalUnitStatus"])["GlobalUnitStatus"]
assert p.outpost_B_status == 2, f"持续激活只维持临时控制,应保持 2,实际 {p.outpost_B_status}"
control("speed", 5)
a = read(["GlobalUnitStatus", "GlobalSpecialMechanism"], 1.3, lambda: control("zone", 2, 3))
unit = a["GlobalUnitStatus"]
assert unit.base_shield == 2, f"R 基地阵地被蓝方临时控制,己方基地阵地应为 2,实际 {unit.base_shield}"
assert 5 in a["GlobalSpecialMechanism"].mechanism_id, "临时控制的阵地要报激活计时(机制 5)"
control("start")
time.sleep(1.5)
b = read(["GlobalUnitStatus"])["GlobalUnitStatus"]
print("阵地:", unit.outpost_R_status, unit.outpost_B_status, unit.base_shield, unit.enemy_base_shield)
print("控制度:", unit.total_CP_ally, unit.total_CP_enemy, "->", b.total_CP_ally, b.total_CP_enemy)
assert unit.outpost_R_status == 1, f"R 前哨阵地红方完全控制 = 己方完全 1,实际 {unit.outpost_R_status}"
assert unit.enemy_base_shield == 1, f"B 基地阵地蓝方完全控制 = 对方完全 1,实际 {unit.enemy_base_shield}"
assert b.base_shield == 1, f"临时控制 5 秒未激活应回退为己方完全控制 1,实际 {b.base_shield}"
assert b.total_CP_ally > unit.total_CP_ally, "红方(己方)控制度应增长"
assert b.total_CP_enemy > unit.total_CP_enemy, "蓝方(对方)控制度应增长"
print("PASS: 阵地状态按己方/对方编码上报,控制度按阵地类型累计")
EOF

echo "[4/7] 建筑血量、堡垒、我方基地被攻击"
$PY sim/match_control.py speed 1 >/dev/null
$PY sim/match_control.py pause >/dev/null
$PY - <<'EOF'
exec(open("/tmp/rm_accept_probe.py").read())
def act():
    control("structure", 0, 1234)   # 红方基地
    control("structure", 2, 777)    # R 前哨站
    control("fortress", 1)          # 蓝方开始激活红方堡垒
got = read(["GlobalUnitStatus", "GlobalSpecialMechanism", "Event"], 1.5, act)
unit, special = got["GlobalUnitStatus"], got["GlobalSpecialMechanism"]
print("基地/前哨:", unit.base_health, unit.outpost_R_health, " 机制:", list(special.mechanism_id),
      list(special.mechanism_time_sec), " 事件:", got["Event"])
assert unit.base_health == 1234, f"己方基地血量应为 1234,实际 {unit.base_health}"
assert unit.outpost_R_health == 777, f"R 前哨站血量应为 777,实际 {unit.outpost_R_health}"
assert 1 in special.mechanism_id, "红方堡垒被激活,对红方席位是机制 1(己方堡垒被对方激活)"
assert (11, "") in got["Event"], "我方基地掉血要发 Event 11"
control("fortress", 0)
print("PASS: 建筑血量、堡垒激活计时、基地被攻击事件")
EOF

echo "[5/7] 飞镖命中(手动注入)"
$PY - <<'EOF'
exec(open("/tmp/rm_accept_probe.py").read())
got = read(["Event"], 1.0, lambda: control("blind", 1, 4))   # 红方中「基地随机移动目标」
print("事件:", got["Event"])
assert (9, "2,4") in got["Event"], f"红方被蓝方飞镖命中 = Event 9「2,4」,实际 {got['Event']}"
print("PASS: 飞镖命中只发 Event 9,遮挡时长交给终端按规则算")
EOF
$PY sim/match_control.py blind-clear >/dev/null

echo "[6/7] 飞镖开闸/发射(走闸门和冷却规则)"
$PY sim/match_control.py zone 1 4 >/dev/null
$PY sim/match_control.py seek 34 >/dev/null   # 比赛内 30s,第一个开闸窗口
$PY sim/match_control.py dart-open 1 >/dev/null
$PY sim/match_control.py seek 41 >/dev/null   # 比赛内 37s,7 秒开闸完成
$PY - <<'EOF'
exec(open("/tmp/rm_accept_probe.py").read())
got = read(["GlobalUnitStatus", "Event"], 1.5, lambda: control("dart", 1, 1))
unit = got["GlobalUnitStatus"]
print("飞镖后:", unit.outpost_B_health, unit.outpost_B_status, " 事件:", got["Event"])
assert unit.outpost_B_health == 0, f"击毁后 B 前哨站应倒下,实际 {unit.outpost_B_health}"
assert unit.outpost_B_status == 3, f"蓝方完全控制被击毁降为蓝方临时 = 对方临时 3,实际 {unit.outpost_B_status}"
for expected in ((2, "111"), (9, "1,1"), (6, "3")):
    assert expected in got["Event"], f"缺少事件 {expected},实际 {got['Event']}"
for second in range(42, 47):
    control("seek", second)
    time.sleep(0.3)
b = read(["GlobalUnitStatus"])["GlobalUnitStatus"]
print("5 秒未激活后:", b.outpost_B_health, b.outpost_B_status)
assert b.outpost_B_health == 500, f"5s 未激活后应重建 500 HP,实际 {b.outpost_B_health}"
assert b.outpost_B_status == 1, f"5s 未激活后转红方完全控制 = 己方完全 1,实际 {b.outpost_B_status}"
print("PASS: 开闸窗口内发射飞镖,击毁后按表 5-21 转化并重建")
EOF

echo "[7/7] 结算:基地被击毁"
$PY - <<'EOF'
exec(open("/tmp/rm_accept_probe.py").read())
control("structure", 1, 0)          # 蓝方基地
time.sleep(0.6)
game = read(["GameStatus"])["GameStatus"]
print("结算:", game.current_stage, game.game_result, game.end_reason, game.red_score)
assert game.current_stage == 5 and game.game_result == 1 and game.end_reason == 1, \
    "蓝方基地被击毁:进入结算,红方胜,原因 1"
print("PASS: 结算阶段报胜方和结束原因")
EOF
echo "ALL PASS"
