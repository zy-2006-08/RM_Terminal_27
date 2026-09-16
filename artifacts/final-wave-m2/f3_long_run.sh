#!/usr/bin/env bash
# F3 long runtime: covers BOTH blind windows of the deterministic timeline.
#
# Covers plan F3 items (1) exactly 4 transitions, (2) both video durations
# ~= 12s blind + 3s hysteresis, (3) no flapping inside either window,
# (5) decoder residency across switches, (9) runtime zero-control-publish.
#
# Timeline (sim/match_server.py:67 _BLIND_SCHEDULE, match-internal time; stage 4
# starts at simulator +10s):
#   blind #1 match [30s,42s)  -> terminal enters ~+40s, returns ~+55s
#   blind #2 match [150s,162s) -> terminal enters ~+160s, returns ~+175s
# So the run must reach ~+185s. A 170s run would truncate the second exit and
# report 3 transitions, which reads as a missed switch rather than a short run.
#
# Item 5 note, stated honestly: the GUI has no periodic frame-counter sink, so this
# script does NOT read decoded_frames at runtime. It proves decoder RESIDENCY --
# the ffmpeg child PID never changes across both switches, and no reconnect_udp /
# decoder-recovery record appears. Monotonicity of the counter itself is a static
# property (video_receiver.cpp:142 is the only write and it is `++`; there is no
# reset anywhere), verified by grep in the audit step, not here.
set -u -o pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

BIN="${RM_TERMINAL_BINARY:-$REPO_ROOT/build/macos/rm_terminal}"
PY="${RM_TERMINAL_PYTHON:-$REPO_ROOT/.venv/bin/python}"
OUT="$REPO_ROOT/artifacts/final-wave-m2/long-run"
BROKER_PORT=3333
RUN_SECONDS="${F3_RUN_SECONDS:-190}"

fail() { printf 'FAIL %s\n' "$*" >&2; exit 1; }

[ -x "$BIN" ] || fail "no terminal binary at $BIN"
[ -x "$PY" ] || fail "no venv python at $PY"
pgrep -f "[m]atch_server.py" >/dev/null 2>&1 && fail "a match_server is already running; refusing"
pgrep -f "[v]ideo_sender.py" >/dev/null 2>&1 && fail "a video_sender is already running; refusing"
nc -z 127.0.0.1 "$BROKER_PORT" 2>/dev/null || fail "no broker on $BROKER_PORT"

rm -rf "$OUT"; mkdir -p "$OUT"

WIRE_PID=""; SIM_PID=""; VID_PID=""; TERM_PID=""
cleanup() {
    for pid in "$TERM_PID" "$VID_PID" "$SIM_PID" "$WIRE_PID"; do
        [ -n "${pid:-}" ] || continue
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
}
trap cleanup EXIT INT TERM

# The wildcard subscriber must be attached BEFORE any publisher so the capture has
# no blind spot at the front. Subscribing afterwards would miss exactly the window
# where a rogue startup publish would appear.
mosquitto_sub -h 127.0.0.1 -p "$BROKER_PORT" -t '#' -v > "$OUT/wire.log" 2>"$OUT/wire.err" &
WIRE_PID=$!
sleep 1
kill -0 "$WIRE_PID" 2>/dev/null || fail "wildcard subscriber failed: $(cat "$OUT/wire.err")"
echo "== wildcard subscriber attached (pid $WIRE_PID) before any publisher"

"$PY" -u sim/match_server.py > "$OUT/match_server.log" 2>&1 &
SIM_PID=$!
SIM_T0="$($PY -c 'import time; print(time.time())')"
for _ in $(seq 1 100); do
    grep -q "模拟赛事引擎启动" "$OUT/match_server.log" 2>/dev/null && break
    kill -0 "$SIM_PID" 2>/dev/null || fail "match_server died: $(cat "$OUT/match_server.log")"
    sleep 0.1
done
grep -q "模拟赛事引擎启动" "$OUT/match_server.log" 2>/dev/null || fail "match_server never became ready"
echo "== match_server up (pid $SIM_PID) at epoch $SIM_T0"

"$PY" -u sim/video_sender.py > "$OUT/video_sender.log" 2>&1 &
VID_PID=$!
echo "== video_sender up (pid $VID_PID)"

cat > "$OUT/run.conf" <<CONF
log_destination = $OUT/rm_terminal.log
CONF

QT_QPA_PLATFORM=offscreen "$BIN" --config "$OUT/run.conf" > "$OUT/terminal_stdout.log" 2>&1 &
TERM_PID=$!
echo "== terminal up (pid $TERM_PID); observing through +${RUN_SECONDS}s"

# Decoder residency sampling. The ffmpeg child is spawned by the terminal, so its
# PID changing is exactly what a decoder restart looks like from outside.
: > "$OUT/decoder-pids.txt"
DEADLINE="$($PY -c "print($SIM_T0 + $RUN_SECONDS)")"
while :; do
    now="$($PY -c 'import time; print(time.time())')"
    [ "$($PY -c "print(1 if $now >= $DEADLINE else 0)")" = "1" ] && break
    kill -0 "$TERM_PID" 2>/dev/null || fail "terminal exited early; stdout:
$(cat "$OUT/terminal_stdout.log")"
    kill -0 "$SIM_PID" 2>/dev/null || fail "match_server exited early"
    kill -0 "$VID_PID" 2>/dev/null || fail "video_sender exited early"
    child="$(pgrep -P "$TERM_PID" 2>/dev/null | tr '\n' ',' || true)"
    printf '%.2f %s\n' "$($PY -c "print($now - $SIM_T0)")" "${child:-none}" >> "$OUT/decoder-pids.txt"
    sleep 2
done
echo "== observation window closed"

cp "$OUT/rm_terminal.log" "$OUT/asserted_rm_terminal.log"
cp "$OUT/wire.log" "$OUT/asserted_wire.log"

RM_SIM_T0="$SIM_T0" RM_LOG="$OUT/asserted_rm_terminal.log" \
RM_WIRE="$OUT/asserted_wire.log" RM_PIDS="$OUT/decoder-pids.txt" \
RM_SIM_LOG="$OUT/match_server.log" "$PY" - <<'PY'
import os
import re
import sys
from datetime import datetime, timezone

sim_t0 = float(os.environ["RM_SIM_T0"])

BLIND_STARTS = (40.0, 160.0)      # terminal-visible entry, = match 30s/150s + 10s stage offset
EXPECTED_DELTA = 12.0 + 3.0       # blind duration + exit hysteresis
ENTER_TOLERANCE = 2.0
DELTA_TOLERANCE = 1.5

records = []
with open(os.environ["RM_LOG"], encoding="utf-8") as handle:
    for line in handle:
        if "event=ui_mode_switch" not in line:
            continue
        fields = dict(re.findall(r"(\w+)=([^\s]+)", line))
        stamp = datetime.strptime(fields["ts"], "%Y-%m-%dT%H:%M:%S.%fZ").replace(
            tzinfo=timezone.utc
        )
        records.append(
            {
                "at": stamp.timestamp() - sim_t0,
                "from": fields.get("from"),
                "to": fields.get("to"),
                "reason": fields.get("reason"),
                "freshness": fields.get("blind_freshness"),
            }
        )

failures = []


def report(label, ok, actual, expected):
    print(
        "{mark}  {label}\n      actual:   {actual}\n      expected: {expected}".format(
            mark="PASS" if ok else "FAIL", label=label, actual=actual, expected=expected
        )
    )
    if not ok:
        failures.append(label)


print()
print("simulator blind timeline (simulator's own stdout):")
for line in open(os.environ["RM_SIM_LOG"], encoding="utf-8"):
    if "致盲" in line:
        print("  " + line.strip())

print()
print("observed ui_mode_switch records (seconds after simulator start):")
for record in records or [None]:
    if record is None:
        print("  (none)")
        break
    print(
        "  +{at:7.2f}s  {frm} -> {to:<5}  reason={reason} freshness={freshness}".format(
            at=record["at"], frm=record["from"], to=record["to"],
            reason=record["reason"], freshness=record["freshness"],
        )
    )

print()
print("assertions:")

# (1) exactly four transitions: two blind windows, two transitions each.
report(
    "exactly 4 mode transitions across both blind windows",
    len(records) == 4,
    "{} transition(s)".format(len(records)),
    "4 (enter/exit for each of 2 blind windows)",
)

if len(records) != 4:
    print()
    print("cannot check pairing or timing without exactly 4 records.")
    sys.exit(1)

pairs = [(records[0], records[1]), (records[2], records[3])]

for index, (enter, leave) in enumerate(pairs, start=1):
    report(
        "window {}: enters video because blind was asserted".format(index),
        enter["to"] == "Video" and enter["reason"] == "BlindAsserted",
        "to={} reason={}".format(enter["to"], enter["reason"]),
        "to=Video reason=BlindAsserted",
    )
    report(
        "window {}: returns to info through the hysteresis path".format(index),
        leave["to"] == "Info" and leave["reason"] == "BlindClearedHysteresis",
        "to={} reason={}".format(leave["to"], leave["reason"]),
        "to=Info reason=BlindClearedHysteresis",
    )
    expected_at = BLIND_STARTS[index - 1]
    report(
        "window {}: entry lands at the scheduled instant".format(index),
        abs(enter["at"] - expected_at) <= ENTER_TOLERANCE,
        "+{:.2f}s".format(enter["at"]),
        "{:.1f}s +/- {:.1f}s".format(expected_at, ENTER_TOLERANCE),
    )
    delta = leave["at"] - enter["at"]
    report(
        "window {}: video lasts blind duration plus exit hysteresis".format(index),
        abs(delta - EXPECTED_DELTA) <= DELTA_TOLERANCE,
        "{:.2f}s".format(delta),
        "{:.1f}s +/- {:.1f}s".format(EXPECTED_DELTA, DELTA_TOLERANCE),
    )
    inside = [r for r in records if enter["at"] < r["at"] < leave["at"]]
    report(
        "window {}: no additional transitions inside the window".format(index),
        not inside,
        "{} intermediate transition(s)".format(len(inside)),
        "0",
    )

# (5) decoder residency. A restart shows up as the child PID set changing.
pid_samples = []
with open(os.environ["RM_PIDS"], encoding="utf-8") as handle:
    for line in handle:
        at, _, pids = line.strip().partition(" ")
        pid_samples.append((float(at), pids))
observed = {pids for _, pids in pid_samples if pids not in ("none", "")}
report(
    "the decoder child process is the same one for the whole run",
    len(observed) == 1,
    "{} distinct child-pid set(s): {}".format(len(observed), sorted(observed)),
    "1 (decoder resident, never respawned across either switch)",
)
covering = [at for at, pids in pid_samples if pids not in ("none", "")]
report(
    "decoder samples span both blind windows",
    bool(covering) and min(covering) < BLIND_STARTS[0] and max(covering) > BLIND_STARTS[1] + EXPECTED_DELTA,
    "samples from +{:.0f}s to +{:.0f}s".format(min(covering), max(covering)) if covering else "none",
    "before +{:.0f}s through after +{:.0f}s".format(BLIND_STARTS[0], BLIND_STARTS[1] + EXPECTED_DELTA),
)

restart_events = []
with open(os.environ["RM_LOG"], encoding="utf-8") as handle:
    for line in handle:
        if "event=reconnect_udp" in line or "event=decode_failure" in line:
            restart_events.append(line.strip())
report(
    "no decoder teardown or failure record during the run",
    not restart_events,
    "{} record(s)".format(len(restart_events)),
    "0 reconnect_udp / decode_failure records",
)

# (9) runtime zero-control observation, scoped honestly: this proves no control
# TOPIC carried traffic from anyone. It does not by itself identify publishers;
# the terminal's read-only property rests on that plus the static/binary audit.
topics = {}
with open(os.environ["RM_WIRE"], encoding="utf-8") as handle:
    for line in handle:
        topic = line.split(" ", 1)[0].strip()
        if topic:
            topics[topic] = topics.get(topic, 0) + 1
control_topics = {t: c for t, c in topics.items() if "CustomControl" in t or "CommonCommand" in t}
print()
print("wire topics observed for the whole run:")
for topic, count in sorted(topics.items(), key=lambda kv: -kv[1]):
    print("  {:<24} {}".format(topic, count))
report(
    "no control topic carried any message at any point in the run",
    not control_topics,
    "{}".format(control_topics or "none"),
    "no CustomControl / CommonCommand messages",
)
report(
    "the capture is non-vacuous (inbound families really were observed)",
    len(topics) >= 5 and sum(topics.values()) > 100,
    "{} topic(s), {} message(s)".format(len(topics), sum(topics.values())),
    ">=5 topics and >100 messages, proving the subscriber was live",
)

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
