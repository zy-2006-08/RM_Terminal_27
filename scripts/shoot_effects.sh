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

# 指挥手大局的一段走势:红方先完全控制蓝方前哨阵地 30 秒(每秒 +1),再丢掉、蓝方完全控制红方前哨阵地
# 60 秒。10 倍速跑完,临时控制 5 秒不激活会退回去,所以只用完全控制。
overview_story() {
    ctl zone 0 2
    ctl zone 1 4
    ctl zone 2 2
    ctl zone 3 4
    sleep 3
    ctl zone 1 2
    ctl speed 10
    sleep 3
    ctl zone 1 4
    ctl zone 0 4
    sleep 6
    ctl speed 1
}

# 第四块:指挥手的大局,再把同一局打完看赛后复盘网页。
scene_overview() {
    start_sim 6
    for size in 1440x900 1280x760 1920x1080; do
        ctl seek 200
        capture 6 "$size" 19000 "commander-$size.png"
        overview_story
        done_shot "commander $size"
    done

    ctl seek 200
    capture 6 1440x900 40000 commander-settlement-1440x900.png
    overview_story
    ctl seek 412
    done_shot settlement
    stop_sim
    local report
    report=$(ls -t "$XDG_DATA_HOME"/*/reports/*.html 2>/dev/null | head -1)
    [ -n "$report" ] || fail "没写出复盘报告"
    cp "$report" "$OUT/report-sample.html"
    google-chrome --headless=new --disable-gpu --hide-scrollbars --window-size=1100,2600 \
        --screenshot="$OUT/report-sample.png" "file://$OUT/report-sample.html" > /dev/null 2>&1
    echo "report -> $(basename "$report")"
}

# 第五块:地图事件动画。动画只放两三秒,每种单拍一张,事件掐在截图前一点点发。
scene_map_anim() {
    start_sim 3
    capture 3 1440x900 7000 dart-1440x900.png
    sleep 6.0
    ctl blind 2 2
    done_shot dart

    capture 3 1440x900 7000 gate-1440x900.png
    sleep 5.2
    ctl dart-open 2
    done_shot gate

    # 敌方的能量机关要 3 秒才分得出是谁开的。
    capture 3 1440x900 9000 rune-1440x900.png
    sleep 3.5
    ctl enemy-rune small
    done_shot rune

    capture 3 1440x900 7000 base-hit-1440x900.png
    sleep 5.4
    ctl structure 0 9200
    done_shot base-hit

    capture 3 1440x900 7000 zone-1440x900.png
    sleep 4.8
    ctl zone 1 2
    done_shot zone

    capture 3 1440x900 9000 fortress-1440x900.png
    sleep 3
    ctl fortress 1
    done_shot fortress
    stop_sim
}

# 第六块:录像片段、MP4、PDF、多局对比。终端这回要录像(不加 --no-record),打两局,
# 每局结束写复盘;再把第 1 局的片段导成 MP4。
scene_replay() {
    export RM_SETTLEMENT_SEC=5
    start_sim 3
    RM_SELF_ID=3 QT_QPA_PLATFORM=offscreen RM_TERMINAL_CAPTURE_SIZE=1440x900 RM_TERMINAL_CAPTURE_DELAY_MS=80000 \
        "$BIN" --config "$RUN/rm.conf" --screenshot "$OUT/live-1440x900.png" > "$RUN/logs/terminal-live.log" 2>&1 &
    SHOT=$!
    PIDS+=("$SHOT")
    sleep 4
    ctl intrude 103 tunnel
    sleep 6
    ctl zone 1 2
    sleep 5
    ctl blind 2 2
    sleep 5
    ctl seek 412
    sleep 18
    ctl auto 0
    ctl seek 200
    sleep 3
    ctl intrude 104 highland
    sleep 6
    ctl zone 0 4
    sleep 6
    ctl seek 412
    done_shot live
    stop_sim

    local reports="$XDG_DATA_HOME/rm_terminal/reports"
    local first
    first=$(ls "$reports"/*第1局.json 2>/dev/null | head -1)
    [ -n "$first" ] || fail "没写出第 1 局的复盘"
    for _ in $(seq 1 40); do ls "$reports"/*.pdf > /dev/null 2>&1 && break; sleep 0.5; done
    RM_TERMINAL_CAPTURE_SIZE=1280x760 bash scripts/export_clips.sh "$first" || fail "导片段失败"
    rm -rf "$OUT/reports"
    cp -r "$reports" "$OUT/reports"
    local stem
    stem=$(basename "$first" .json)
    google-chrome --headless=new --disable-gpu --hide-scrollbars --window-size=1100,3000 \
        --screenshot="$OUT/report-1.png" "file://$OUT/reports/$stem.html" > /dev/null 2>&1
    google-chrome --headless=new --disable-gpu --hide-scrollbars --window-size=1140,760 \
        --screenshot="$OUT/index.png" "file://$OUT/reports/index.html" > /dev/null 2>&1
    ffmpeg -y -loglevel error -ss 9 -i "$OUT/reports/clips/$stem-01.mp4" -frames:v 1 "$OUT/clip-01-frame.png"
    echo "reports -> artifacts/$BLOCK/reports ($(ls "$OUT/reports/clips" | wc -l) clips)"
}

declare -F "scene_$BLOCK" > /dev/null || fail "用法: bash scripts/shoot_effects.sh {buffs|radar|field|overview|map_anim|replay}"
mkdir -p "$OUT"
mosquitto -p "$P" > "$RUN/logs/mosquitto.log" 2>&1 &
PIDS+=("$!")
for _ in $(seq 1 50); do nc -z 127.0.0.1 "$P" 2>/dev/null && break; sleep 0.1; done
echo "port $P -> artifacts/$BLOCK/ (logs: $RUN/logs)"
"scene_$BLOCK"
