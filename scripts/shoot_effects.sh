#!/usr/bin/env bash
# 每块新功能的效果截图，存到仓库 artifacts/<块名>/（不放 /tmp，系统会清）。
#
#   bash scripts/shoot_effects.sh buffs
#
# broker 起在随机私有端口，模拟器靠 RM_MQTT_PORT 连过去，不碰本机 3333 那一套。
# 界面方案用 scripts/effects_ui_profiles.json：步兵、重装是「新手 · 带下一步」，指挥手是「标准」。
set -u -o pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"
BLOCK="${1:-}"
PY="${RM_TERMINAL_PYTHON:-$REPO/.venv/bin/python}"
BIN="${RM_TERMINAL_BINARY:-$REPO/build/ubuntu/rm_terminal}"
OUT="$REPO/artifacts/$BLOCK"
RUN="$(mktemp -d "${TMPDIR:-/tmp}/rm-shoot-effects.XXXXXX")"
P=$((4500 + RANDOM % 400))

fail() { printf 'FAIL %s\n' "$*" >&2; exit 1; }
[ -x "$BIN" ] || fail "terminal binary not found: $BIN"
[ -x "$PY" ] || fail "simulator python not found: $PY"

export RM_MQTT_PORT=$P XDG_CONFIG_HOME="$RUN/cfg" XDG_DATA_HOME="$RUN/data"
mkdir -p "$XDG_CONFIG_HOME/rm_terminal" "$XDG_DATA_HOME" "$RUN/logs"
cp scripts/effects_ui_profiles.json "$XDG_CONFIG_HOME/rm_terminal/ui_profiles.json"
cat > "$RUN/rm.conf" <<CONF
mqtt_host = 127.0.0.1
mqtt_port = $P
udp_port = $((P + 1))
video_wall_port = $((P + 10))
log_destination = $RUN/logs/rm_terminal.log
speech_dir = $RUN/no-voice
CONF

PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT INT TERM

ctl() { "$PY" -m sim.match_control "$@" > /dev/null 2>&1; }
# publish <主题> <protobuf 表达式>：替终端按键，比如 publish RuneActivateCommand 'pb.RuneActivateCommand(activate=1)'。
publish() {
    "$PY" - "$1" "$2" > /dev/null 2>&1 <<'PY'
import os, sys
sys.path.insert(0, "generated")
import paho.mqtt.client as mqtt
import rm_terminal_pb2 as pb
topic, expr = sys.argv[1], sys.argv[2]
c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="shoot-effects")
c.connect("127.0.0.1", int(os.environ["RM_MQTT_PORT"]))
c.loop_start()
c.publish(topic, eval(expr, {"pb": pb}).SerializeToString()).wait_for_publish(5)
c.loop_stop()
c.disconnect()
PY
}
# 开赛后跳到第 200 秒、关随机掉血，画面上的数才固定。
start_sim() {
    RM_SELF_ID=$1 RM_PREP_SEC=1 RM_CHECK_SEC=1 RM_COUNTDOWN_SEC=1 \
        "$PY" -u -m sim.match_server > "$RUN/logs/sim-$1.log" 2>&1 &
    SIM=$!
    PIDS+=("$SIM")
    for _ in $(seq 1 100); do grep -q "模拟赛事引擎启动" "$RUN/logs/sim-$1.log" 2>/dev/null && break; sleep 0.1; done
    sleep 7
    ctl auto 0
    ctl seek 200
    sleep 1
}
stop_sim() { kill "$SIM" 2>/dev/null; wait "$SIM" 2>/dev/null; }
# capture <席位> <宽x高> <几毫秒后截> <文件名>：后台起离屏终端，截完自己退出；进程号在 SHOT。
capture() {
    RM_SELF_ID=$1 QT_QPA_PLATFORM=offscreen RM_TERMINAL_CAPTURE_SIZE=$2 RM_TERMINAL_CAPTURE_DELAY_MS=$3 \
        "$BIN" --no-record --config "$RUN/rm.conf" --screenshot "$OUT/$4" > "$RUN/logs/terminal-$4.log" 2>&1 &
    SHOT=$!
    PIDS+=("$SHOT")
}
done_shot() { wait "$SHOT"; echo "$1 -> $?"; }

# 第一块：增益。
scene_buffs() {
    # 步兵：大能量机关（攻击×3）+ 敌方小机关 + 地形 + 敌方 4 号复活 + 雷达标记 + 蓝方基地 300 血
    # 第一块拍的是没有雷达情报时按规则推的样子，模拟器的雷达情报先关掉。
    start_sim 3
    ctl radar-intel off
    ctl structure 1 300
    ctl buy 17 100
    ctl zone 1 2
    for size in 1440x900 1280x760 1920x1080; do
        capture 3 "$size" 20000 "infantry-$size.png"
        sleep 3
        ctl enemy-rune small
        ctl terrain highland
        ctl terrain through
        ctl radar-lock 104
        sleep 1
        publish RuneActivateCommand 'pb.RuneActivateCommand(activate=1)'
        sleep 4
        ctl hp 104 0
        sleep 2
        ctl hp 104 20
        done_shot "infantry $size"
    done
    stop_sim

    # 指挥手：敌方小机关 + 哨兵防御姿态 + 雷达标记
    start_sim 6
    ctl zone 1 2
    for size in 1440x900 1280x760 1920x1080; do
        capture 6 "$size" 13000 "commander-$size.png"
        sleep 3
        ctl enemy-rune small
        ctl radar-lock 104
        publish SentryCtrlCommand 'pb.SentryCtrlCommand(command_id=8)'
        done_shot "commander $size"
    done
    stop_sim

    # 堡垒提醒：我方护甲未展开时补「那台易伤100%」
    start_sim 3
    capture 3 1440x900 9000 infantry-fortress-1440x900.png
    sleep 4
    ctl fortress 1
    done_shot fortress
    stop_sim
}

# 敌方 4 号读条复活：打掉以后读条立刻走完，复活后无敌且虚弱。
revive_enemy_4() {
    ctl hp 104 0
    sleep 1
    ctl revive 104
}

# 第二块：雷达情报。同一局面拍有情报、没情报、情报断了三种。
scene_radar() {
    start_sim 3
    for size in 1440x900 1280x760 1920x1080; do
        revive_enemy_4
        capture 3 "$size" 9000 "infantry-$size.png"
        sleep 3
        ctl enemy-rune small
        ctl radar-lock 104
        done_shot "infantry $size"
    done

    ctl radar-intel off
    revive_enemy_4
    capture 3 1440x900 9000 infantry-no-intel-1440x900.png
    sleep 3
    ctl enemy-rune small
    ctl radar-lock 104
    done_shot "no intel"

    ctl radar-intel on
    revive_enemy_4
    capture 3 1440x900 12000 infantry-stale-1440x900.png
    sleep 3
    ctl enemy-rune small
    ctl radar-lock 104
    sleep 2
    ctl radar-intel off
    done_shot stale
    stop_sim

    # 重装也收得到；指挥手没有 0x0310，还是按规则推（等官方组网答疑）。
    start_sim 1
    revive_enemy_4
    capture 1 1440x900 9000 hero-1440x900.png
    sleep 3
    ctl enemy-rune small
    done_shot hero
    stop_sim
}

# 第三块：场地坐标。入侵横幅三条路各一张，再摆位置看按位置推的增益和偷堡垒的是谁。
scene_field() {
    start_sim 3
    for route in ${FIELD_ROUTES-highland tunnel road}; do
        ctl place 103 off
        capture 3 1440x900 8500 "intrusion-$route-1440x900.png"
        sleep 4
        ctl intrude 103 "$route"
        done_shot "intrusion $route"
    done
    ctl place 103 off

    # 按位置推的标签要关掉雷达情报才看得到（有情报时敌方用实际数）。
    ctl radar-intel off
    ctl place 4 10.6 7.5
    ctl place 7 2.5 7.5
    ctl place 104 17.4 7.5
    ctl place 101 27.0 14.0
    ctl place 103 6.6 7.5
    for size in 1440x900 1280x760 1920x1080; do
        # 堡垒提醒在「开始激活」那一刻报,终端要先开着、看到 3 号已经站在我方堡垒上。
        ctl fortress 0
        capture 3 "$size" 7000 "positions-$size.png"
        sleep 4
        ctl fortress 1
        done_shot "positions $size"
    done
    stop_sim

    start_sim 6
    capture 6 1440x900 8500 commander-intrusion-1440x900.png
    sleep 4
    ctl intrude 103 tunnel
    done_shot "commander intrusion"
    stop_sim
}

declare -F "scene_$BLOCK" > /dev/null || fail "用法: bash scripts/shoot_effects.sh {buffs|radar|field}"
mkdir -p "$OUT"
mosquitto -p "$P" > "$RUN/logs/mosquitto.log" 2>&1 &
PIDS+=("$!")
for _ in $(seq 1 50); do nc -z 127.0.0.1 "$P" 2>/dev/null && break; sleep 0.1; done
echo "port $P -> artifacts/$BLOCK/ (logs: $RUN/logs)"
"scene_$BLOCK"
