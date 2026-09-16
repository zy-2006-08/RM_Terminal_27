#!/usr/bin/env bash
# F3 item 6 probe: what does video mode ACTUALLY paint once the sender stops?
#
# The plan requires "停掉 sim/video_sender.py 后 video 模式显示离线文字". Reading
# cpp/video_receiver.cpp says that cannot happen: `latest_` is written at line 142
# and cleared nowhere (stopDecoder() clears only `raw_`), so latestFrame() keeps
# returning the last decoded frame forever. VideoPane::paintEvent draws that image
# and returns BEFORE the status text (cpp/dashboard.cpp:304-309), so the pane would
# hold a frozen picture instead. This script decides it by pixels, not by reading.
#
# Three captures, all in --force-mode video, all exiting through the normal capture
# callback so the exit code is a real exit code and not a signal:
#   A  sender alive throughout          -> expect a painted frame
#   B  sender killed mid-run            -> the question under test
#   C  sender never started             -> expect the status placard
# C is the control that proves the classifier can see a placard at all; without it
# a "frame" verdict in B could just mean the classifier never reports placards.
#
# EXACTLY ONE sender may exist at a time, and the previous one must be reaped before
# the next starts. An earlier version of this script leaked A's sender into B and C,
# which fed UDP 3334 for runs that were supposed to be silent and made all three
# captures read as "frame" -- a false negative that looked like a real finding.
set -u -o pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

BIN="${RM_TERMINAL_BINARY:-$REPO_ROOT/build/macos/rm_terminal}"
PY="${RM_TERMINAL_PYTHON:-$REPO_ROOT/.venv/bin/python}"
OUT="$REPO_ROOT/artifacts/final-wave-m2/offline-probe"
BROKER_PORT=3333
UDP_PORT=3334

fail() { printf 'FAIL %s\n' "$*" >&2; exit 1; }

[ -x "$BIN" ] || fail "no terminal binary at $BIN"
[ -x "$PY" ] || fail "no venv python at $PY"
pgrep -f "[m]atch_server.py" >/dev/null 2>&1 && fail "a match_server is already running; refusing"
pgrep -f "[v]ideo_sender.py" >/dev/null 2>&1 && fail "a video_sender is already running; refusing"
nc -z 127.0.0.1 "$BROKER_PORT" 2>/dev/null || fail "no broker on $BROKER_PORT"

rm -rf "$OUT"; mkdir -p "$OUT"

SIM_PID=""
VID_PID=""
cleanup() {
    for pid in "$VID_PID" "$SIM_PID"; do
        [ -n "${pid:-}" ] || continue
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
}
trap cleanup EXIT INT TERM

# grep -c exits 1 when the count is zero, so a bare `|| echo 0` appends a second
# line and the field becomes "0\n0". Take the count and normalise separately.
count_event() {
    local file="$1" event="$2" n
    n="$(grep -c "event=$event" "$file" 2>/dev/null || true)"
    printf '%s' "${n:-0}"
}

"$PY" -u sim/match_server.py > "$OUT/match_server.log" 2>&1 &
SIM_PID=$!
for _ in $(seq 1 100); do
    grep -q "模拟赛事引擎启动" "$OUT/match_server.log" 2>/dev/null && break
    kill -0 "$SIM_PID" 2>/dev/null || fail "match_server died: $(cat "$OUT/match_server.log")"
    sleep 0.1
done
grep -q "模拟赛事引擎启动" "$OUT/match_server.log" 2>/dev/null || fail "match_server never became ready"
echo "== match_server up (pid $SIM_PID)"

start_sender() {
    [ -z "$VID_PID" ] || fail "internal: start_sender called while sender $VID_PID is still tracked"
    "$PY" -u sim/video_sender.py >> "$OUT/video_sender.log" 2>&1 &
    VID_PID=$!
    echo "== video_sender up (pid $VID_PID)"
}

stop_sender() {
    [ -n "$VID_PID" ] || return 0
    kill "$VID_PID" 2>/dev/null || true
    wait "$VID_PID" 2>/dev/null || true
    echo "== video_sender $VID_PID stopped and reaped"
    VID_PID=""
    # The socket must be quiet before the next run starts, or that run inherits
    # this run's frames and its verdict is meaningless.
    sleep 2
    if pgrep -f "[v]ideo_sender.py" >/dev/null 2>&1; then
        fail "a video_sender survived stop_sender; refusing to continue"
    fi
}

assert_no_sender() {
    pgrep -f "[v]ideo_sender.py" >/dev/null 2>&1 && fail "$1: a video_sender is running but must not be"
    # Nothing should be bound to the feed port either, terminal excluded (it binds
    # to receive). Recorded rather than asserted: the terminal itself holds it.
    lsof -nP -iUDP:"$UDP_PORT" > "$OUT/$1-udp-holders.txt" 2>&1 || true
    echo "== $1: confirmed no video_sender process"
}

# Each capture gets its own config so its log is its own; the default log opens in
# append mode and would blend the three runs together.
run_capture() {
    local tag="$1" delay_ms="$2" kill_at="$3"
    local conf="$OUT/$tag.conf"
    cat > "$conf" <<CONF
log_destination = $OUT/$tag.log
CONF
    QT_QPA_PLATFORM=offscreen RM_TERMINAL_CAPTURE_DELAY_MS="$delay_ms" \
        "$BIN" --config "$conf" --force-mode video \
        --screenshot "$OUT/$tag.png" --dump-layout "$OUT/$tag.json" \
        > "$OUT/$tag.stdout" 2>&1 &
    local term_pid=$!
    if [ "$kill_at" != "-" ]; then
        sleep "$kill_at"
        stop_sender
        echo "== $tag: sender stopped ${kill_at}s into a ${delay_ms}ms capture"
    fi
    wait "$term_pid"
    local status=$?
    echo "$status" > "$OUT/$tag.exit"
    echo "== $tag exit=$status"
}

# A: sender alive for the whole capture.
start_sender
sleep 4                       # let the decoder spawn and produce frames
run_capture A 8000 -
stop_sender

# B: sender dies 8s into a 22s capture, leaving ~14s of silence before the shot.
assert_no_sender B-pre
start_sender
sleep 4
run_capture B 22000 8

# C: control. No sender ever ran during this capture.
assert_no_sender C-pre
run_capture C 8000 -

echo
echo "== receiver records (proves the receiver noticed the feed state)"
printf '%-4s %-14s %-16s %s\n' tag reconnect_udp decode_failure exit
for tag in A B C; do
    printf '%-4s %-14s %-16s %s\n' "$tag" \
        "$(count_event "$OUT/$tag.log" reconnect_udp)" \
        "$(count_event "$OUT/$tag.log" decode_failure)" \
        "$(cat "$OUT/$tag.exit")"
done

echo
echo "== pane content by pixels"
"$PY" artifacts/final-wave-m2/f3_png.py "$OUT/A.png" "$OUT/B.png" "$OUT/C.png"
