#!/usr/bin/env bash
# End-to-end mode regression: proves the WIRED system honours the hysteresis rules
# that cpp/ui_mode_test.cpp proves at the unit level. A unit-green machine fed the
# wrong clock or the wrong freshness still flaps, and only a real run catches that.
#
# Timeline it relies on (sim/match_server.py:67 _BLIND_SCHEDULE):
#   stage()==4 begins at sim elapsed 10s; blind runs match-internal [30s, 42s).
#   So, relative to simulator start: blind ASSERTS at +40s, CLEARS at +52s, and the
#   terminal's 3000ms exit hysteresis puts the return to info at ~+55s.
# The run therefore has to reach +60s to observe the exit at all.
#
# Exits non-zero on any failed assertion, and prints an actual-vs-expected table.
set -u -o pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

BINARY="${RM_TERMINAL_BINARY:-$REPO_ROOT/build/macos/rm_terminal}"
VENV_PY="${RM_TERMINAL_PYTHON:-$REPO_ROOT/.venv/bin/python}"
BROKER_PORT=3333
RUN_DIR="$(mktemp -d "${TMPDIR:-/tmp}/rm-mode-regression.XXXXXX")"
TERMINAL_LOG="$RUN_DIR/rm_terminal.log"
CONF="$RUN_DIR/rm_terminal.conf"

# Only PIDs this script starts are ever signalled. Killing by name would take out a
# developer's own simulator, or an unrelated python process.
STARTED_PIDS=()
cleanup() {
    for pid in "${STARTED_PIDS[@]:-}"; do
        [ -n "${pid:-}" ] || continue
        kill "$pid" 2>/dev/null || true
    done
    for pid in "${STARTED_PIDS[@]:-}"; do
        [ -n "${pid:-}" ] || continue
        wait "$pid" 2>/dev/null || true
    done
}
trap cleanup EXIT INT TERM

fail() { printf 'FAIL %s\n' "$*" >&2; exit 1; }

[ -x "$BINARY" ] || fail "terminal binary not found or not executable: $BINARY
  build it first: cmake --build build/macos --target rm_terminal
  (rm_terminal is the executable; terminal_dashboard is a static library)"
[ -x "$VENV_PY" ] || fail "simulator python not found: $VENV_PY
  create it first: python3 -m venv .venv && .venv/bin/pip install protobuf paho-mqtt"

# A second match_server on the same broker publishes a second blind timeline at a
# different phase, which manufactures extra transitions and would make this gate
# report flapping that the terminal never caused. Refuse rather than emit junk.
if pgrep -f "[m]atch_server.py" > /dev/null 2>&1; then
    fail "a match_server.py is already running; its blind timeline would overlap this
  run and corrupt the transition count. Stop it first (this script only cleans up
  processes it started itself)."
fi

echo "== run directory: $RUN_DIR"

# The broker may legitimately already be up; only start one if it is not, and only
# then take responsibility for stopping it.
if nc -z 127.0.0.1 "$BROKER_PORT" 2>/dev/null; then
    echo "== broker already listening on $BROKER_PORT (left running)"
else
    command -v mosquitto > /dev/null 2>&1 || fail "no broker on $BROKER_PORT and mosquitto is not installed"
    mosquitto -p "$BROKER_PORT" > "$RUN_DIR/mosquitto.log" 2>&1 &
    STARTED_PIDS+=("$!")
    echo "== started broker (pid $!)"
    for _ in $(seq 1 50); do
        nc -z 127.0.0.1 "$BROKER_PORT" 2>/dev/null && break
        sleep 0.1
    done
    nc -z 127.0.0.1 "$BROKER_PORT" 2>/dev/null || fail "broker never came up on $BROKER_PORT"
fi

# -u matters: without it Python block-buffers stdout when redirected, so the
# readiness line never arrives and the blind-timeline lines are lost on signal.
"$VENV_PY" -u sim/match_server.py > "$RUN_DIR/match_server.log" 2>&1 &
SIM_PID=$!
STARTED_PIDS+=("$SIM_PID")

# T0 is read AFTER the fork but before readiness, so it can only overestimate the
# simulator's age, never underestimate it. Every deadline below derives from it.
SIM_T0="$($VENV_PY -c 'import time; print(time.time())')"
echo "== started match_server (pid $SIM_PID) at epoch $SIM_T0"

for _ in $(seq 1 100); do
    grep -q "模拟赛事引擎启动" "$RUN_DIR/match_server.log" 2>/dev/null && break
    kill -0 "$SIM_PID" 2>/dev/null || fail "match_server died during startup:
$(cat "$RUN_DIR/match_server.log")"
    sleep 0.1
done
grep -q "模拟赛事引擎启动" "$RUN_DIR/match_server.log" 2>/dev/null \
    || fail "match_server never reported readiness:
$(cat "$RUN_DIR/match_server.log")"

"$VENV_PY" -u sim/video_sender.py > "$RUN_DIR/video_sender.log" 2>&1 &
STARTED_PIDS+=("$!")
echo "== started video_sender (pid $!)"

# A dedicated log: the default rm_terminal.log opens in APPEND mode, so parsing it
# would also see every previous run's transitions.
cat > "$CONF" <<CONF
log_destination = $TERMINAL_LOG
CONF

QT_QPA_PLATFORM=offscreen "$BINARY" --config "$CONF" > "$RUN_DIR/terminal_stdout.log" 2>&1 &
TERMINAL_PID=$!
STARTED_PIDS+=("$TERMINAL_PID")
echo "== started terminal (pid $TERMINAL_PID), observing through +62s"

# Run past the exit transition (~+55s) with margin, and keep proving the terminal is
# alive: a crash at +45s would otherwise read as "no extra transitions".
DEADLINE="$($VENV_PY -c "print($SIM_T0 + 62)")"
while :; do
    now="$($VENV_PY -c 'import time; print(time.time())')"
    done_yet="$($VENV_PY -c "print(1 if $now >= $DEADLINE else 0)")"
    [ "$done_yet" = "1" ] && break
    kill -0 "$TERMINAL_PID" 2>/dev/null || fail "terminal exited early; stdout:
$(cat "$RUN_DIR/terminal_stdout.log")"
    kill -0 "$SIM_PID" 2>/dev/null || fail "match_server exited early; log:
$(cat "$RUN_DIR/match_server.log")"
    sleep 1
done

echo "== observation window closed; asserting"
[ -f "$TERMINAL_LOG" ] || fail "terminal wrote no log at $TERMINAL_LOG"
cp "$TERMINAL_LOG" "$RUN_DIR/asserted_rm_terminal.log"

RM_SIM_T0="$SIM_T0" RM_LOG="$RUN_DIR/asserted_rm_terminal.log" \
RM_SIM_LOG="$RUN_DIR/match_server.log" "$VENV_PY" - <<'PY'
import os
import re
import sys
from datetime import datetime, timezone

sim_t0 = float(os.environ["RM_SIM_T0"])
log_path = os.environ["RM_LOG"]
sim_log_path = os.environ["RM_SIM_LOG"]

# Expected, derived from sim/match_server.py:67 and the 3000ms default hysteresis.
BLIND_ASSERT_AT = 40.0
BLIND_DURATION = 12.0
EXIT_HYSTERESIS = 3.0
ENTER_TOLERANCE = 2.0
DELTA_TOLERANCE = 1.5
expected_delta = BLIND_DURATION + EXIT_HYSTERESIS

with open(log_path, encoding="utf-8") as handle:
    lines = handle.readlines()

records = []
for line in lines:
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

print()
print("simulator blind timeline (from the simulator's own stdout):")
for line in open(sim_log_path, encoding="utf-8"):
    if "致盲" in line:
        print("  " + line.strip())

print()
print("observed ui_mode_switch records (seconds after simulator start):")
if not records:
    print("  (none)")
for record in records:
    print(
        "  +{at:6.2f}s  {frm} -> {to:<5}  reason={reason} freshness={freshness}".format(
            at=record["at"],
            frm=record["from"],
            to=record["to"],
            reason=record["reason"],
            freshness=record["freshness"],
        )
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
print("assertions:")

# (1) exactly two transitions. More than two IS the flap signal.
report(
    "exactly 2 mode transitions in the run",
    len(records) == 2,
    "{} transition(s)".format(len(records)),
    "2 (one into video, one back to info)",
)

if len(records) != 2:
    print()
    print(
        "cannot check ordering or timing without exactly 2 records; "
        "{} extra/missing transition(s) already indicate flapping or a missed switch.".format(
            abs(len(records) - 2)
        )
    )
    sys.exit(1)

enter, leave = records

# (2) direction and reason of each transition.
report(
    "first transition enters video because blind was asserted",
    enter["to"] == "Video" and enter["reason"] == "BlindAsserted",
    "to={} reason={}".format(enter["to"], enter["reason"]),
    "to=Video reason=BlindAsserted",
)
report(
    "second transition returns to info through the hysteresis path",
    leave["to"] == "Info" and leave["reason"] == "BlindClearedHysteresis",
    "to={} reason={}".format(leave["to"], leave["reason"]),
    "to=Info reason=BlindClearedHysteresis",
)

# (3) the entry lands where the simulator's schedule says it must.
report(
    "video is entered about {:.0f}s after simulator start".format(BLIND_ASSERT_AT),
    abs(enter["at"] - BLIND_ASSERT_AT) <= ENTER_TOLERANCE,
    "+{:.2f}s".format(enter["at"]),
    "{:.1f}s +/- {:.1f}s".format(BLIND_ASSERT_AT, ENTER_TOLERANCE),
)

# (4) the gap is the blind duration PLUS the hysteresis, not either alone. This is
# the assertion that fails if the hysteresis is misconfigured or bypassed.
delta = leave["at"] - enter["at"]
report(
    "video lasts the blind window plus the exit hysteresis",
    abs(delta - expected_delta) <= DELTA_TOLERANCE,
    "{:.2f}s".format(delta),
    "{:.1f}s ({:.0f}s blind + {:.0f}s hysteresis) +/- {:.1f}s".format(
        expected_delta, BLIND_DURATION, EXIT_HYSTERESIS, DELTA_TOLERANCE
    ),
)

# (5) nothing happened between them. Redundant while the count is 2, but it states
# the property directly so a future relaxation of (1) cannot silently drop it.
inside = [r for r in records if enter["at"] < r["at"] < leave["at"]]
report(
    "no additional transitions inside the blind window",
    not inside,
    "{} intermediate transition(s)".format(len(inside)),
    "0",
)

print()
if failures:
    print("RESULT: FAIL ({} assertion(s) failed)".format(len(failures)))
    for label in failures:
        print("  - " + label)
    sys.exit(1)
print("RESULT: PASS (mode switching is correctly timed and does not flap)")
PY
STATUS=$?

if [ "$STATUS" -ne 0 ]; then
    echo "== artifacts kept for diagnosis: $RUN_DIR" >&2
    exit "$STATUS"
fi
echo "== artifacts: $RUN_DIR"
exit 0
