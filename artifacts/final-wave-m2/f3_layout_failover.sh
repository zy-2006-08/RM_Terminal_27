#!/usr/bin/env bash
# F3 items 4, 6, 7: layout gates with all three processes, and clean exits when a
# dependency dies.
#
#   item 4  forced info + forced video captures with match_server AND video_sender
#           both alive, re-running todo 12's four layout assertions. (The archived
#           task-12 captures were taken with the broker and video only, so the
#           three-process requirement was never actually covered.)
#   item 6  video_sender stopped -> info mode's map still renders, exit code 0
#   item 7  match_server stopped -> terminal survives, exit code 0
#
# Exit codes come from the normal capture callback, never from a signal: a process
# killed with SIGTERM tells you nothing about whether it can shut down cleanly.
set -u -o pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

BIN="${RM_TERMINAL_BINARY:-$REPO_ROOT/build/macos/rm_terminal}"
PY="${RM_TERMINAL_PYTHON:-$REPO_ROOT/.venv/bin/python}"
OUT="$REPO_ROOT/artifacts/final-wave-m2/layout-failover"
BROKER_PORT=3333

fail() { printf 'FAIL %s\n' "$*" >&2; exit 1; }

[ -x "$BIN" ] || fail "no terminal binary at $BIN"
[ -x "$PY" ] || fail "no venv python at $PY"
pgrep -f "[m]atch_server.py" >/dev/null 2>&1 && fail "a match_server is already running; refusing"
pgrep -f "[v]ideo_sender.py" >/dev/null 2>&1 && fail "a video_sender is already running; refusing"
nc -z 127.0.0.1 "$BROKER_PORT" 2>/dev/null || fail "no broker on $BROKER_PORT"

rm -rf "$OUT"; mkdir -p "$OUT"

SIM_PID=""; VID_PID=""
cleanup() {
    for pid in "$VID_PID" "$SIM_PID"; do
        [ -n "${pid:-}" ] || continue
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
}
trap cleanup EXIT INT TERM

start_sim() {
    "$PY" -u sim/match_server.py > "$OUT/match_server.log" 2>&1 &
    SIM_PID=$!
    for _ in $(seq 1 100); do
        grep -q "模拟赛事引擎启动" "$OUT/match_server.log" 2>/dev/null && break
        kill -0 "$SIM_PID" 2>/dev/null || fail "match_server died: $(cat "$OUT/match_server.log")"
        sleep 0.1
    done
    grep -q "模拟赛事引擎启动" "$OUT/match_server.log" 2>/dev/null || fail "match_server never became ready"
    echo "== match_server up (pid $SIM_PID)"
}
stop_sim() {
    [ -n "$SIM_PID" ] || return 0
    kill "$SIM_PID" 2>/dev/null || true
    wait "$SIM_PID" 2>/dev/null || true
    echo "== match_server $SIM_PID stopped and reaped"
    SIM_PID=""
    sleep 1
}
start_sender() {
    [ -z "$VID_PID" ] || fail "internal: sender $VID_PID still tracked"
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
    sleep 2
    pgrep -f "[v]ideo_sender.py" >/dev/null 2>&1 && fail "a video_sender survived stop_sender"
    return 0
}

capture() {
    local tag="$1" mode="$2" delay_ms="$3"
    local conf="$OUT/$tag.conf"
    cat > "$conf" <<CONF
log_destination = $OUT/$tag.log
CONF
    QT_QPA_PLATFORM=offscreen RM_TERMINAL_CAPTURE_DELAY_MS="$delay_ms" \
        "$BIN" --config "$conf" --force-mode "$mode" \
        --screenshot "$OUT/$tag.png" --dump-layout "$OUT/$tag.json" \
        > "$OUT/$tag.stdout" 2>&1
    local status=$?
    echo "$status" > "$OUT/$tag.exit"
    echo "== $tag (force-mode $mode) exit=$status"
}

# ---- item 4: three processes alive for both captures -------------------------
start_sim
start_sender
sleep 4                                   # let the decoder produce frames
echo "== item 4: capturing with match_server + video_sender + terminal all alive"
ps -p "$SIM_PID" -o pid=,comm= > "$OUT/item4-processes.txt" 2>&1 || true
ps -p "$VID_PID" -o pid=,comm= >> "$OUT/item4-processes.txt" 2>&1 || true
capture item4-info info 6000
capture item4-video video 6000

# ---- item 6: video feed dies, info mode must still work ----------------------
stop_sender
echo "== item 6: capturing info mode with the video feed dead"
capture item6-info-no-video info 6000

# ---- item 7: match server dies, terminal must survive and exit cleanly -------
stop_sim
echo "== item 7: capturing with the match server dead"
capture item7-no-match info 6000

echo
echo "== assertions"
RM_OUT="$OUT" "$PY" - <<'PY'
import json
import os
import subprocess
import sys

# f3_png.py sits in the parent of the run directory (artifacts/final-wave-m2/).
sys.path.insert(0, os.path.dirname(os.environ["RM_OUT"]))
import f3_png

out = os.environ["RM_OUT"]
failures = []


def report(label, ok, actual, expected):
    print("{mark}  {label}\n      actual:   {actual}\n      expected: {expected}".format(
        mark="PASS" if ok else "FAIL", label=label, actual=actual, expected=expected))
    if not ok:
        failures.append(label)


def load(tag):
    with open(os.path.join(out, tag + ".json"), encoding="utf-8") as handle:
        dump = json.load(handle)
    dump["_panes"] = {p["name"]: p for p in dump["panes"]}
    return dump


def area(pane):
    return pane["width"] * pane["height"]


def exit_code(tag):
    with open(os.path.join(out, tag + ".exit"), encoding="utf-8") as handle:
        return int(handle.read().strip())


def shutdown_records(tag):
    path = os.path.join(out, tag + ".log")
    with open(path, "rb") as handle:
        return [l for l in handle.read().split(b"\n") if b"event=shutdown" in l]


print("--- item 4: layout gates with all three processes alive ---")
info, video = load("item4-info"), load("item4-video")

report("both three-process captures exited 0 and wrote non-empty artifacts",
       all(exit_code(t) == 0 for t in ("item4-info", "item4-video")) and
       all(os.path.getsize(os.path.join(out, t + e)) > 0
           for t in ("item4-info", "item4-video") for e in (".png", ".json")),
       "exits={} sizes={}".format(
           [exit_code(t) for t in ("item4-info", "item4-video")],
           [os.path.getsize(os.path.join(out, t + e))
            for t in ("item4-info", "item4-video") for e in (".png", ".json")]),
       "exit 0 for both; all 4 files non-empty")

map_pane, thumb = info["_panes"]["map_pane"], info["_panes"]["info_video_pane"]
report("info mode: map is visible and dominates the video thumbnail 4:1",
       map_pane["visible"] and area(map_pane) > 4 * area(thumb),
       "map={} ({}px) thumbnail={} ({}px) ratio={:.2f}".format(
           map_pane["visible"], area(map_pane), thumb["visible"], area(thumb),
           area(map_pane) / area(thumb) if area(thumb) else float("inf")),
       "map visible and > 4x the thumbnail area")

full = video["_panes"]["video_full_pane"]
window = video["window"]["width"] * video["window"]["height"]
report("video mode: feed fills >60% of the window and the map is hidden",
       full["visible"] and area(full) > 0.60 * window and not video["_panes"]["map_pane"]["visible"],
       "video_full={} ({}px, {:.1%} of window) map_visible={}".format(
           full["visible"], area(full), area(full) / window,
           video["_panes"]["map_pane"]["visible"]),
       ">60% of window, map_pane hidden")

report("read-only banner is visible in BOTH modes with the forced reason recorded",
       info["readonly_banner_visible"] and video["readonly_banner_visible"] and
       info["mode"] == "Info" and video["mode"] == "Video" and
       info["reason"] == "ForcedByCli" and video["reason"] == "ForcedByCli",
       "info(banner={} mode={} reason={}) video(banner={} mode={} reason={})".format(
           info["readonly_banner_visible"], info["mode"], info["reason"],
           video["readonly_banner_visible"], video["mode"], video["reason"]),
       "banner true in both; modes Info/Video; reason ForcedByCli in both")

differ = subprocess.run(["cmp", "-s", os.path.join(out, "item4-info.png"),
                         os.path.join(out, "item4-video.png")]).returncode != 0
report("the two mode screenshots really differ (force-mode is not a no-op)",
       differ, "differ" if differ else "byte-identical", "the PNGs must differ")

print()
print("--- item 6: info mode with the video feed dead ---")
no_video = load("item6-info-no-video")
mp, th = no_video["_panes"]["map_pane"], no_video["_panes"]["info_video_pane"]
report("info mode still lays the map out as the primary area with no video feed",
       mp["visible"] and area(mp) > 4 * area(th),
       "map={} ({}px) thumbnail={} ({}px)".format(mp["visible"], area(mp), th["visible"], area(th)),
       "map visible and still dominant")

# The map must have really drawn something, not just occupied a rectangle. A flat
# fill would be a handful of colours; field lines, markers and the status band are
# hundreds.
png = f3_png.read_png(os.path.join(out, "item6-info-no-video.png"))
box = f3_png.centre_crop(png)
stats = f3_png.region_stats(png, *box)
report("the map actually rendered content rather than a blank rectangle",
       stats["distinct_colours"] >= 200,
       "{} distinct colours in {} (dominant {} at {:.1%})".format(
           stats["distinct_colours"], box, stats["dominant"], stats["dominant_fraction"]),
       ">=200 distinct colours, i.e. real drawing")

report("info mode exits 0 when the video feed is gone",
       exit_code("item6-info-no-video") == 0,
       "exit={}".format(exit_code("item6-info-no-video")), "0")
recs = shutdown_records("item6-info-no-video")
report("that run wrote a shutdown record with exit=0 (a real exit, not a signal)",
       any(b"exit=0" in r for r in recs),
       "{} shutdown record(s): {}".format(len(recs), [r.decode("utf-8", "replace")[-40:] for r in recs]),
       "a shutdown record carrying exit=0")

print()
print("--- item 7: the match server is gone ---")
report("the terminal survives the match server dying and exits 0",
       exit_code("item7-no-match") == 0,
       "exit={}".format(exit_code("item7-no-match")), "0")
recs = shutdown_records("item7-no-match")
report("that run wrote a shutdown record with exit=0",
       any(b"exit=0" in r for r in recs),
       "{} shutdown record(s)".format(len(recs)), "a shutdown record carrying exit=0")
no_match = load("item7-no-match")
report("the read-only banner survives loss of the match server",
       no_match["readonly_banner_visible"],
       "readonly_banner_visible={}".format(no_match["readonly_banner_visible"]), "true")

print()
if failures:
    print("RESULT: FAIL ({} assertion(s) failed)".format(len(failures)))
    for label in failures:
        print("  - " + label)
    sys.exit(1)
print("RESULT: PASS")
PY
STATUS=$?
echo "== artifacts: $OUT"
exit "$STATUS"
