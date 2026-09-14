# Native task-2 foundation

This document covers only the native CMake target `rm_terminal`; it does not
describe or replace the legacy Python/PySide6 launcher (`main.py`, `run.sh`) or
its simulation modules. The native target is a C++17/Qt6 read-only placeholder.

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
| `stale_window_ms` | `500` | Field freshness window; older fields report stale |
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
| `5` | evidence capture failure (`--screenshot` could not write the PNG) |

Command-line flags:

| Flag | Meaning |
| --- | --- |
| `--config <path>` | Configuration file path; highest precedence |
| `--safe-smoke` | Report the read-only safe state and exit without a window |
| `--screenshot <path>` | Render the window to a PNG, then exit |
| `--diagnostic <host> <port> <seconds>` | Bounded headless MQTT observation |

`--screenshot` renders the widget itself with `QWidget::grab()` rather than
capturing the screen, so it needs no recording permission, captures nothing but
this window, and works under `QT_QPA_PLATFORM=offscreen`. The `shutdown` record
carries the code the process actually returns, so a failed capture logs
`exit=5`, not `exit=0`.

`RM_TERMINAL_CAPTURE_DELAY_MS` (default `600`) delays that capture. The video
pane needs the decoder to spawn and complete one frame, so a screenshot meant to
show video needs roughly `7000`; the default is fine for telemetry-only shots.

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
