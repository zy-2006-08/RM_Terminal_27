#!/usr/bin/env bash
# 三种席位(步兵 3、重装 1、指挥手 6)× 两档分辨率,各出一张截图和一份布局 JSON,
# 存到 artifacts/roles/。终端连的是本地模拟器,所以图里是模拟器演的比赛数据。
#
#   bash scripts/shoot_roles.sh
#
# 自己起 broker(3333 没人监听时)、模拟器和图传,只清理自己起的进程。约 1.5 分钟。
set -u -o pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

case "$(uname -s)" in
    Darwin) BUILD_DIR=build/macos ;;
    *)      BUILD_DIR=build/ubuntu ;;
esac
BINARY="${RM_TERMINAL_BINARY:-$REPO_ROOT/$BUILD_DIR/rm_terminal}"
VENV_PY="${RM_TERMINAL_PYTHON:-$REPO_ROOT/.venv/bin/python}"
OUT="$REPO_ROOT/artifacts/roles"
RUN_DIR="$(mktemp -d "${TMPDIR:-/tmp}/rm-shoot-roles.XXXXXX")"
mkdir -p "$OUT"

STARTED_PIDS=()
cleanup() {
    for pid in "${STARTED_PIDS[@]:-}"; do
        [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null
    done
    wait 2>/dev/null
}
trap cleanup EXIT INT TERM
fail() { printf 'FAIL %s\n' "$*" >&2; exit 1; }

[ -x "$BINARY" ] || fail "terminal binary not found: $BINARY"
[ -x "$VENV_PY" ] || fail "simulator python not found: $VENV_PY"
if pgrep -f "[m]atch_server" > /dev/null 2>&1; then
    fail "a match_server is already running; stop it first (its seat would not match)"
fi

if ! nc -z 127.0.0.1 3333 2>/dev/null; then
    mosquitto -p 3333 > "$RUN_DIR/mosquitto.log" 2>&1 &
    STARTED_PIDS+=("$!")
    for _ in $(seq 1 50); do nc -z 127.0.0.1 3333 2>/dev/null && break; sleep 0.1; done
fi
"$VENV_PY" -u sim/video_sender.py > "$RUN_DIR/video_sender.log" 2>&1 &
STARTED_PIDS+=("$!")

cat > "$RUN_DIR/rm_terminal.conf" <<CONF
log_destination = $RUN_DIR/rm_terminal.log
speech_dir = $RUN_DIR/no-voice
CONF

for seat in 3 1 6; do
    case "$seat" in
        1) role=hero ;;
        6) role=commander ;;
        *) role=infantry ;;
    esac
    RM_SELF_ID=$seat RM_PREP_SEC=1 RM_CHECK_SEC=1 RM_COUNTDOWN_SEC=1 \
        "$VENV_PY" -u -m sim.match_server > "$RUN_DIR/sim-$seat.log" 2>&1 &
    SIM_PID=$!
    STARTED_PIDS+=("$SIM_PID")
    for _ in $(seq 1 100); do
        grep -q "模拟赛事引擎启动" "$RUN_DIR/sim-$seat.log" 2>/dev/null && break
        sleep 0.1
    done
    # 让比赛跑起来:开赛后几秒才有事件、阵地和装配/飞镖状态可看。
    sleep 8
    for size in 1440x900 1920x1080; do
        RM_SELF_ID=$seat QT_QPA_PLATFORM=offscreen RM_TERMINAL_CAPTURE_SIZE=$size \
            RM_TERMINAL_CAPTURE_DELAY_MS=4000 "$BINARY" --no-record --config "$RUN_DIR/rm_terminal.conf" \
            --screenshot "$OUT/$role-$size.png" --dump-layout "$OUT/$role-$size.json" \
            > "$RUN_DIR/terminal-$role-$size.log" 2>&1 \
            || fail "capture $role $size failed: $(cat "$RUN_DIR/terminal-$role-$size.log")"
        grep -q "\"role\": \"$role\"" "$OUT/$role-$size.json" || fail "$role-$size.json does not name the role"
        echo "== $role $size -> $OUT/$role-$size.png"
    done
    kill "$SIM_PID" 2>/dev/null
    wait "$SIM_PID" 2>/dev/null
done
echo "== logs: $RUN_DIR"
