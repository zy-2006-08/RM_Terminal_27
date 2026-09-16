# Native task-2 foundation

This document covers the native CMake target `rm_terminal`, which is now the
only terminal implementation. The legacy Python/PySide6 launcher (`main.py`,
`core/`, `ui/`) has been removed; `run.sh` starts the native binary against the
Python simulator in `sim/`, which remains the only data source for local runs
and for `mode_regression_e2e`. The native target is C++17/Qt6 and read-only.

Task 2 uses the six inbound message families in `proto/rm_terminal.proto`:
GameStatus, RobotDynamicStatus, RobotModuleStatus, RobotPosition, Event, and
RobotTelemetry. The C++ inbound vocabulary is a LOCAL SIMULATION schema
adapter only; it is not an official RM2027 wire contract or a protobuf
decoder. Optional fields are patches: omitted fields do not change state,
invalid fields preserve their last valid value but report invalid quality, and
each field has independent freshness. Robot updates require an explicit source
identity unless the message embeds one. Events and match state are global.

Freshness uses caller-provided monotonic milliseconds, expires at age >= a
positive threshold, and rejects backwards time. Every `MonotonicMs` in the
process comes from `rm_terminal::monotonic_now()` (`cpp/clock.h`) — one
`steady_clock` origin shared by MQTT intake, `Store::snapshot`, and the UI tick.
Wall-clock time must never be used: `Store::snapshot` throws when handed an
earlier instant, so an NTP correction backward would terminate the process
mid-match, and a forward jump would let the mode exit window "complete" across
a sleep during which no clear samples were observed. Two CTest cases enforce
this: `monotonic_clock` asserts the clock is process-relative and never
retreats, and `monotonic_clock_guard` fails the build if
`currentMSecsSinceEpoch` reappears in `cpp/`. `domain_test` uses explicit
checks rather than `assert`, so Release retains all semantic verification.
It has no MQTT, UDP, CAN, UART, or other control transport. Platform-specific
selection is limited to compile-time identity in `cpp/platform.h`; core
application code uses Qt and standard C++ APIs.

## Explicit task-1 boundary

The native application exposes only a visible read-only simulation-safe state.
It must not connect to transports, publish commands, send device data, decode
telemetry, or implement later-task protocol/state/video features. The legacy
Python files remain excluded from this native build and are not changed by this
task.

## Configuration

Runtime settings come from a `key=value` file; no source edit or rebuild is
required to change them. Copy `rm_terminal.conf.sample` and edit it.

| Key | Default | Meaning |
| --- | --- | --- |
| `mqtt_host` | `127.0.0.1` | MQTT broker host for the local simulator |
| `mqtt_port` | `3333` | MQTT broker port (2026 protocol baseline) |
| `udp_port` | `3334` | H.265 video UDP listen port (2026 protocol baseline) |
| `stale_window_ms` | `2200` | Field freshness window; older fields report stale |
| `log_level` | `info` | Minimum level written: `debug`, `info`, `warning`, `error` |
| `log_destination` | `rm_terminal.log` | Log file path; records append across restarts |
| `mode_exit_hysteresis_ms` | `3000` | Consecutive clear time before leaving video mode |
| `event_history_capacity` | `50` | Bounded event ring size; oldest records drop first |
| `blind_stale_fallback_ms` | `15000` | Stale-blind fallback delay; **`0` disables it** |
| `map_enabled` | `true` | Draw the tactical map pane: `true` or `false` |

Every integer key rejects zero and negatives except `blind_stale_fallback_ms`,
which accepts `0` to mean "never fall back to video just because blind telemetry
went stale". `-1` is still rejected. An unknown key is a hard failure with exit
code 4 naming the key and its line number; it is never silently ignored.

Precedence, highest first:

1. `--config <path>`
2. `RM_TERMINAL_CONFIG` environment variable
3. `./rm_terminal.conf`
4. built-in defaults from the table above

A missing config file is intentionally not an error: the defaults are used so a
fresh checkout runs unconfigured. A file that exists but contains an unknown
key, an empty value, a non-positive integer, or an unknown log level fails
loudly and names the offending key, value, and line number.

Exit codes, stable across tasks 1-6:

| Code | Meaning |
| --- | --- |
| `0` | success |
| `2` | malformed command-line arguments |
| `3` | transport start failure (MQTT intake) |
| `4` | configuration or log-destination failure |
| `5` | evidence capture failure (`--screenshot` could not write the PNG, or `--dump-layout` could not write the JSON) |

Command-line flags:

| Flag | Meaning |
| --- | --- |
| `--config <path>` | Configuration file path; highest precedence |
| `--safe-smoke` | Report the read-only safe state and exit without a window |
| `--screenshot <path>` | Render the window to a PNG, then exit |
| `--diagnostic <host> <port> <seconds>` | Bounded headless MQTT observation |
| `--force-mode <info\|video>` | Debug/evidence only: pin the UI mode (see below) |
| `--dump-layout <path>` | Debug/evidence only: write the layout as JSON (see below) |

`--screenshot`, `--force-mode` and `--dump-layout` are capture and debugging
tools, not operator features. They are documented together under
[调试与证据采集](#调试与证据采集-debug-and-evidence-capture) below.

### Window

The window shows game state (stage, countdown, round, score), per-robot state
(HP, heat, ammo, bullet speed, chassis power, buffer, gimbal yaw/pitch, chassis
mode, vision, auto-aim, target, friction wheel, position), the live H.265 video
pane with packet/frame statistics, and the latest match event. Values that are
absent render as `--`; values that aged past `stale_window_ms` are suffixed
`(过期)` individually, so one stale field never makes fresh neighbours look stale.

The window is read-only. It has no button, shortcut, or transport that can send
anything to a robot.

### Structured log

Each record is one greppable line:

```
ts=<ISO8601 UTC with ms> level=<level> event=<token> key=value ...
```

Event tokens: `startup`, `shutdown`, `reconnect_mqtt`, `reconnect_udp`,
`readonly_block`, `decode_failure`, `stale_data`, `packet_loss`,
`ui_mode_switch`.

`ui_mode_switch` carries `from`, `to`, `reason`, and `blind_freshness`, and is
written only when the mode actually changes, not on every 250ms refresh tick.
`reason` distinguishes `BlindAsserted`, `BlindClearedHysteresis`,
`BlindDataStale`, `BlindSignalLost`, `ForcedByCli`, and `Startup`, so holding
the video feed because blind telemetry died reads differently from a real
assertion.

The only log sink is the local file named by `log_destination`. Operator-facing
banners (the read-only notice, argument errors) go to stderr through
`qInfo`/`qCritical`, which is a separate mechanism. The read-only terminal never
opens a network log sink, because that would give it an egress path.

## 双模式行为 (dual-mode behaviour)

The window has two resident modes and switches between them by itself. Info mode
shows the tactical map as the primary area with video as a thumbnail; video mode
gives the video feed the window and hides the map.

The switch is driven by the simulated `BlindStatus` signal, never by an operator:

- Blind asserted (fresh reading, value true) enters video mode immediately. Losing
  the picture late is worse than entering early, so there is no entry delay.
- Blind cleared requires `mode_exit_hysteresis_ms` (default `3000`) of continuous
  clear readings before returning to info mode. Without that delay a signal that
  flickers around the threshold would flip the whole layout several times a second.
- A re-assertion during the exit window cancels it, so the full window restarts
  rather than resuming a partial one.
- Stale blind telemetry HOLDS video mode rather than dropping it, and time measured
  while the data was untrustworthy is not credited toward leaving. If staleness
  lasts `blind_stale_fallback_ms` (default `15000`) the terminal returns to info
  with `reason=BlindSignalLost`, so a dead publisher cannot pin it fullscreen
  forever. Setting that key to `0` disables the fallback entirely, which means the
  terminal will stay in video mode indefinitely once the blind feed dies.

Both modes keep the read-only banner, and the decoder stays resident across
switches: the video pane is never reparented and `VideoReceiver` is never stopped,
so `decoded_frames` keeps climbing instead of restarting from zero.

## 调试与证据采集 (debug and evidence capture)

Everything in this section exists for debugging and evidence capture. None of it is
an operator entry point: there is deliberately no keyboard shortcut, menu item, or
button that changes the mode, because during a match the mode must follow the blind
signal rather than anyone's preference. Normal runs pass none of these flags.

`--screenshot <path>` renders the widget itself with `QWidget::grab()` rather than
capturing the screen, so it needs no recording permission, captures nothing but
this window, and works under `QT_QPA_PLATFORM=offscreen`. The `shutdown` record
carries the code the process actually returns, so a failed capture logs `exit=5`,
not `exit=0`.

`RM_TERMINAL_CAPTURE_DELAY_MS` (default `600`) delays that capture. The video pane
needs the decoder to spawn and complete one frame, so a screenshot meant to show
video needs roughly `7000`; the default is fine for telemetry-only shots.

`--force-mode <info|video>` pins the UI to one mode so a capture is deterministic
instead of depending on when the blind signal happens to arrive:

```bash
QT_QPA_PLATFORM=offscreen RM_TERMINAL_CAPTURE_DELAY_MS=2500 \
  build/macos/rm_terminal --force-mode video --screenshot /tmp/forced-video.png
```

Forcing does not falsify the log. The `ui_mode_switch` record reports
`reason=ForcedByCli`, distinct from the `BlindAsserted` and
`BlindClearedHysteresis` reasons the automatic path emits, so forced evidence is
never mistaken for a real blind event. Because the log records transitions only,
`--force-mode info` emits no record at all when Info is already the startup mode.

The flag takes exactly one value, `info` or `video`. A missing value, an
unrecognised value, a repeated flag, or combining it with `--safe-smoke` or
`--diagnostic` all exit `2`; the last two are rejected rather than ignored because
neither builds a window for the mode to apply to. `cpp/assert_bad_args.cmake`
(CTest `reject_unknown_args`) covers each of those cases.

`--dump-layout <path>` writes the on-screen layout as JSON at the same instant the
screenshot is taken, so the two artifacts describe one moment. It exists because a
PNG can only be judged by eye: the JSON makes "the map dominates in info mode" and
"video fills the window in video mode" assertions a script can check.

```bash
QT_QPA_PLATFORM=offscreen RM_TERMINAL_CAPTURE_DELAY_MS=2500 \
  build/macos/rm_terminal --force-mode video \
    --screenshot /tmp/m-video.png --dump-layout /tmp/m-video.json
```

The dump reports `mode`, `reason`, `readonly_banner_visible`, `stacked_index`, the
window box, and one `{name, visible, x, y, width, height}` entry per key pane, in a
fixed order with integer values, so identical input yields a byte-identical file and
any diff means the layout really moved. Panes the stacked layout is not showing
report zeroed geometry: a widget that was never laid out has a stale `geometry()`
that varies between otherwise identical runs.

It carries layout only, never match data or field coordinates. Mixing telemetry in
would make diffs fail randomly on values that have nothing to do with layout.

Omitting the flag writes no file and costs nothing. A missing path, a repeated flag,
or combining it with `--safe-smoke` or `--diagnostic` all exit `2`, and a path that
cannot be written exits `5`.

### Mode regression test

`scripts/verify_mode_regression.sh` proves the wired system honours the switching
rules above, which `ui_mode_test` cannot: a unit-green state machine fed the wrong
clock or the wrong freshness still flaps. The script starts its own broker (only if
none is listening), match server and video sender, runs the terminal offscreen
against a per-run log, then asserts that the run produced exactly two transitions —
`Info -> Video (BlindAsserted)` then `Video -> Info (BlindClearedHysteresis)` — with
entry near +40s and a gap of the 12s blind window plus the 3s exit hysteresis. It
prints an actual-vs-expected table and exits non-zero on any failure.

```sh
bash scripts/verify_mode_regression.sh
```

It needs about 65 seconds of wall clock, because it has to observe the blind window
and the hysteresis that follows it. To run it through CTest, enable it explicitly:

```sh
cmake -S . -B build/macos -DRM_TERMINAL_INTEGRATION_TESTS=ON
ctest --test-dir build/macos -L integration --output-on-failure
```

Registration is opt-in rather than label-only on purpose. A CTest label selects
(`-L`) and excludes (`-LE`) but does **not** make a registered test skip by default,
so labelling alone would have taken a plain `ctest` run from about 1 second to over
a minute. With the option off (the default) `ctest` runs 12 fast tests; with it on,
`-L integration` selects this one and `-LE integration` excludes it.

The script refuses to run if another `match_server.py` is already publishing, since
a second blind timeline at a different phase manufactures extra transitions and
would report flapping the terminal never caused. It signals only the processes it
started itself, and leaves a pre-existing broker running.

## Protobuf generation

The C++ stubs are generated by the `terminal_proto` CMake target automatically
whenever `proto/rm_terminal.proto` changes. The Python stubs under `generated/`
are checked in, so they must be regenerated by hand after every proto edit:

```sh
protoc --python_out=generated -Iproto proto/rm_terminal.proto
```

`BlindStatus`, `RobotPositionSet`, and `RobotPositionEntry` are LOCAL SIMULATION
vocabulary. RM2027 has published no official protocol, so these are not a wire
contract. Every field is `optional`, which is load-bearing: a missing coordinate
reports `has_x() == false` while a real origin reports `has_x() == true` with
`x == 0.0`, so absent positions are never drawn as the field corner. The inbound
`RobotPosition` message predates them and stays as-is.

No terminal-to-robot or terminal-to-server message may be added. `CustomControl`
and `CommonCommand` exist in the proto but are deliberately never wired into any
code path; `intake_test` asserts the decoder rejects those topics.

## Running the Python simulator

`sim/match_server.py` needs `protobuf` and `paho-mqtt`, which the native build
does not provide. A bare `python3` will fail with `ModuleNotFoundError: google`:

```sh
python3 -m venv .venv
.venv/bin/pip install protobuf paho-mqtt
.venv/bin/python -u sim/match_server.py
```

`.venv/` is gitignored. Use `-u`: without it Python block-buffers stdout when
redirected to a file, so the blind-timeline lines are lost when the process is
signalled, and the reproducibility check below has nothing to compare.

The simulator publishes a deterministic blind timeline (`_BLIND_SCHEDULE`) keyed
to match-internal elapsed time, so two runs are byte-identical:

```sh
.venv/bin/python -u sim/match_server.py > /tmp/run1.log 2>&1
# after 60s, repeat into /tmp/run2.log, then:
diff <(grep 致盲 /tmp/run1.log) <(grep 致盲 /tmp/run2.log)
```

Before trusting any live rate measurement, confirm no stale simulator is already
publishing: `ps aux | grep [m]atch_server`. Two simulators on one broker double
every topic's observed rate and make a correct 5Hz sender look wrong.

## macOS

```sh
cmake -S . -B build/macos
cmake --build build/macos
ctest --test-dir build/macos --output-on-failure
build/macos/rm_terminal --safe-smoke
build/macos/rm_terminal --config rm_terminal.conf
```

Debug and Release verification artifacts:

- `artifacts/task-2/revision-baseline.log` — pre-change baseline
- `artifacts/task-2/failing-first.log` — test failed before `store.h` existed
- `artifacts/task-2/domain-verification.log` — Debug/Release build, CTest,
  smoke, direct domain checks, malformed argument matrix, and cleanup receipt

## Ubuntu (unverified on the macOS QA host)

```sh
sudo apt-get install build-essential cmake qt6-base-dev
cmake -S . -B build/ubuntu
cmake --build build/ubuntu
ctest --test-dir build/ubuntu --output-on-failure
build/ubuntu/rm_terminal --safe-smoke
```

The Ubuntu commands above are the deployment-candidate path only; Ubuntu
execution is unverified in this environment. The smoke mode creates a
`QCoreApplication`, reports the read-only safe state, and exits without opening
a window or network transport.

## 平台验证状态 (platform verification status)

| Platform | Build | CTest | Dynamic run (simulator, dual mode, video) |
| --- | --- | --- | --- |
| macOS (arm64, Homebrew Qt6) | verified | verified, Debug and Release | verified |
| Ubuntu | **DEFERRED** | **DEFERRED** | **DEFERRED** |

Ubuntu verification is DEFERRED: no Ubuntu host, container, or cross-toolchain is
available on the QA machine, so nothing in this repository demonstrates an Ubuntu
build, test run, or dynamic run. The Ubuntu commands are a deployment candidate to
be executed later, and no statement here may be read as an Ubuntu pass. Every
timing number, screenshot, and receipt in `artifacts/` and `.omo/evidence/` was
produced on macOS.
