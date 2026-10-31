# Native task-2 foundation

This document covers the native CMake target `rm_terminal`, which is now the
only terminal implementation. The legacy Python/PySide6 launcher (`main.py`,
`core/`, `ui/`) has been removed; `run.sh` starts the native binary against the
Python simulator in `sim/`, which remains the only data source for local runs
and for `mode_regression_e2e`. The native target is C++17/Qt6.

`proto/rm_terminal.proto` is the official RoboMaster 2027 custom-terminal protocol
(通信协议 V1.0.0, 2026-10-29, chapter 2, messages 2.2.1–2.2.32), copied message by
message; the topic name is the message name. The only deviation is
`RadarInfoToClient`, whose official text has no field number: the proto reads field 1
as `repeated RadarSingleRobotInfo`, and `cpp/decoder.cpp` also accepts the other
plausible encoding (flat packed `uint32` triples). `cpp/decoder.cpp` is the single
place that turns the protocol's seat-relative fields (己方/对方, R/B outpost zones,
ally-first `robot_health`, enemy-first radar, centimetres, decimetres) into the
red/blue, metre-based domain model, so nothing above it knows which side the seat is
on. Optional fields are patches: omitted fields do not change state, invalid fields
preserve their last valid value but report invalid quality, and each field has
independent freshness. Robot messages carry no robot id (2027 sends only the seat's
own robot), except `RobotPosition`, which the commander also receives for the own
sentry (7/107). `Store` is locked: MQTT delivers on its network thread while the UI
reads snapshots on the main thread.

Values the protocol does not send are derived locally and named as such: base and
outpost maximum HP come from the rules manual (10000 / 1000); another robot's
maximum HP is the highest HP seen this round, floored at its class minimum; kills
are counted from Event 1; the blind window is computed from Event 9 (see 双模式行为);
the logistics factor λ comes from base HP (rules 5.6.1).

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
Outbound messages: `CustomControl` (forwarded from the custom controller's serial
port, see [链路状态与自检页](#链路状态与自检页-link-status-and-self-check)), and the
seat-specific commands `MapClickCmd`, `DartCommand`, `SentryCtrlCommand` and
`AssemblyCommand` (see [席位与布局](#席位与布局-seats-and-role-layouts)). Platform-specific
selection is limited to compile-time identity in `cpp/platform.h`; core
application code uses Qt and standard C++ APIs.

## Configuration

Runtime settings come from a `key=value` file; no source edit or rebuild is
required to change them. Copy `rm_terminal.conf.sample` and edit it.

| Key | Default | Meaning |
| --- | --- | --- |
| `mqtt_host` | `127.0.0.1` | MQTT broker host for the local simulator |
| `mqtt_port` | `3333` | MQTT broker port (2027 protocol: server `192.168.12.1:3333`) |
| `udp_port` | `3334` | H.265 video UDP listen port (2027 protocol) |
| `self_robot_id` | (unset) | Seat: `1` hero, `3`/`4` infantry, `6` commander, blue `+100`. Unset = ask with the seat picker; `--seat` and `RM_SELF_ID` override it |
| `stale_window_ms` | `2200` | Field freshness window; older fields report stale |
| `log_level` | `info` | Minimum level written: `debug`, `info`, `warning`, `error` |
| `log_destination` | `rm_terminal.log` | Log file path; records append across restarts |
| `mode_exit_hysteresis_ms` | `3000` | Consecutive clear time before leaving video mode |
| `event_history_capacity` | `50` | Bounded event ring size; oldest records drop first |
| `blind_stale_fallback_ms` | `15000` | Stale-blind fallback delay; **`0` disables it** |
| `map_enabled` | `true` | Draw the tactical map pane: `true` or `false` |
| `controller_serial` | (unset) | Custom controller serial port; unset means this seat has none |
| `controller_baud` | `115200` | Controller baud rate; one of 9600 … 921600. The firmware uses 115200 |
| `mqtt_keepalive_s` | `5` | MQTT keepalive; a pulled cable is detected after about two of these |
| `alerts_disabled` | (none) | Comma list of event alerts to switch off: `outpost`, `control`, `fortress`, `dart`, `energy`, `low_hp`, `base` |
| `alert_control_gap` | `30` | Control-point deficit that triggers the 控制度落后 alert |
| `speech_dir` | (default) | Linux voice install directory; default `~/.local/share/rm_terminal/tts` |
| `map_mark_ms` | `30000` | How long a map mark (sent or received) stays on the map; the last 5 s fade |
| `display_fullscreen` | `false` | Fullscreen on start (hero and commander seats with the official 1080p monitor) |
| `display_screen` | (primary) | Screen name for `display_fullscreen`; the `screens` log record lists the names |

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
| `--seat <id>` | Seat for this run (`1 3 4 6 101 103 104 106`); skips the seat picker. `7`/`107` exit `2` (the sentry has no operator) |
| `--safe-smoke` | Report startup state and exit without a window |
| `--screenshot <path>` | Render the window to a PNG, then exit |
| `--diagnostic <host> <port> <seconds>` | Bounded headless MQTT observation |
| `--force-mode <info\|video>` | Debug/evidence only: pin the UI mode (see below) |
| `--dump-layout <path>` | Debug/evidence only: write the layout as JSON (see below) |

`--screenshot`, `--force-mode` and `--dump-layout` are capture and debugging
tools, not operator features. They are documented together under
[调试与证据采集](#调试与证据采集-debug-and-evidence-capture) below.

### Window

The window shows the seat label, link lights and self-check in the status bar; the
top bar (score, round, countdown, base and outpost HP, base defence state, control
points); four robots per side with HP (and remaining bullets for our side); the
tactical map (own robot from `RobotPosition`, the rest from the radar, map marks);
the event feed (Event 2.2.7 in Chinese, stamped with the match clock); the battle
analysis (our economy, damage, kills, energy mechanism, fortress, the four zones,
control points and logistics); the live H.265 video pane; and the role panes of the
seat (see 席位与布局). Values that are absent render as `--`; values that aged past
`stale_window_ms` are suffixed `(过期)` individually, so one stale field never makes
fresh neighbours look stale.

### Structured log

Each record is one greppable line:

```
ts=<ISO8601 UTC with ms> level=<level> event=<token> key=value ...
```

Event tokens: `startup`, `shutdown`, `reconnect_mqtt`, `reconnect_udp`,
`decode_failure`, `stale_data`, `packet_loss`,
`ui_mode_switch`, `quit_signal`, `link_state`, `controller_link`, `alert`,
`alert_voice`, `seat_cancelled`, `seat_confirmed`, `seat_mismatch`, `screens`,
`display`, `match_event`, `sentry_result`, `dart_status`, `map_mark`,
`map_mark_received`, `arm_feedback`.

`startup` carries `seat`, `role` (`infantry`, `hero`, `commander`) and `source`
(`cli`, `env`, `config`, `picker`, `recording`). `seat_confirmed` / `seat_mismatch`
compare the seat with the robot id the server reports in `RobotStaticStatus`.
`match_event` is every decoded Event (2.2.7) with its id, level and the Chinese text
shown in the event feed; `sentry_result`, `dart_status` (target and gate on change)
and `map_mark_received` record what the server answered, `map_mark` what the
commander sent.

`alert` carries `kind` (an `alerts_disabled` name, or `tactical` for a due tactical
reminder), `level` (`critical`, `warning`, `info`) and the banner `text`.
`alert_voice` reports pre-synthesis of the alert phrases (`state=preparing|ready`,
or `synth_failed` with the phrase and error).

`link_state` carries `link` (`referee`, `car`, `video`, `controller`), `from`, `to`
(`Ok`, `Warn`, `Down`, `Off`), `summary` and `detail`, and is written only when a
link's level or summary changes. `controller_link` records the serial port being
opened, closed or failing to open (`state=open|closed|open_failed`), and at most one
`state=bad_frames total=N` per second.

`quit_signal` records which signal (`2` SIGINT, `15` SIGTERM, `1` SIGHUP) asked the
terminal to exit. The first such signal quits through the event loop so the
recording is closed on a record boundary; a second one uses the default action and
kills the process at once.

`ui_mode_switch` carries `from`, `to`, `reason`, and `blind_freshness`, and is
written only when the mode actually changes, not on every 250ms refresh tick.
`reason` distinguishes `BlindAsserted`, `BlindClearedHysteresis`,
`BlindDataStale`, `BlindSignalLost`, `ForcedByCli`, and `Startup`, so holding
the video feed because blind telemetry died reads differently from a real
assertion.

The only log sink is the local file named by `log_destination`. Operator-facing
banners and argument errors go to stderr through `qInfo`/`qCritical`, which is a
separate mechanism. The terminal never opens a network log sink, because that
would give it a second egress path.

## 双模式行为 (dual-mode behaviour)

The window has two resident modes and switches between them by itself. Info mode
shows the tactical map as the primary area with video as a thumbnail; video mode
gives the video feed the window and hides the map.

The switch is driven by the blind window, never by an operator. The 2027 protocol has
no blind message: the terminal starts the window itself when Event 9 reports an
enemy dart hit, with the rules-manual durations (§5.7.3: 10/5/3/2 s for the 1st–4th
hit on the outpost or a fixed base target this round, 2 s after that; 10 s for every
random-moving or terminal-moving hit; consecutive hits add up). While no window is
open, the reading is as fresh as the server stream (`GameStatus`), because a dead
stream could hide a hit:

- Blind asserted (fresh reading, value true) enters video mode immediately. Losing
  the picture late is worse than entering early, so there is no entry delay.
- Blind cleared requires `mode_exit_hysteresis_ms` (default `3000`) of continuous
  clear readings before returning to info mode. Without that delay a signal that
  flickers around the threshold would flip the whole layout several times a second.
- A re-assertion during the exit window cancels it, so the full window restarts
  rather than resuming a partial one.
- A stale stream HOLDS video mode rather than dropping it, and time measured
  while the data was untrustworthy is not credited toward leaving. If staleness
  lasts `blind_stale_fallback_ms` (default `15000`) the terminal returns to info
  with `reason=BlindSignalLost`, so a dead publisher cannot pin it fullscreen
  forever. Setting that key to `0` disables the fallback entirely, which means the
  terminal will stay in video mode indefinitely once the blind feed dies.

The decoder stays resident across switches: the video pane is never reparented and
`VideoReceiver` is never stopped, so `decoded_frames` keeps climbing instead of
restarting from zero.

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

The dump reports `mode`, `reason`, `stacked_index`, the
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
entry near +34s (the simulated enemy dart hits our base's random moving target at
match second 30, `RM_BLIND_AUTO=1`) and a gap of the 10 s blind window plus the 3 s
exit hysteresis. It prints an actual-vs-expected table and exits non-zero on any
failure.

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
a minute. With the option off (the default) `ctest` runs only the fast tests; with it on,
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

Every field is `optional`, as in the official text, which is load-bearing: a missing
value reports `has_x() == false` while a real zero reports `has_x() == true`, so an
absent position is never drawn at the field corner and absent HP is never drawn as
dead. `intake_test` pins the decoding of every message for a red and a blue seat.

The terminal subscribes to the inbound topics listed in `Decoder::topics()` plus
`CustomByteBlock`. Outbound records (`CustomControl`, commands) are recorded too;
the decoder rejects them, so a recording replays without feeding them back into
match state. The simulator also uses `CommonCommand` (2.2.25) as its own match-control
channel, with command types from `0x10` so they never collide with the protocol's 1–6.

## 链路状态与自检页 (link status and self-check)

Four lights sit in the banner row, to the right of the alert strip: 裁判 (MQTT),
车端 (`CustomByteBlock`, the robot's 0x0310 data — in 2027 the only data the robot
itself sends to the terminal), 图传 and 控制器. A green dot
means fine; red adds two or three words saying what is wrong; grey means the seat
does not use that link. Hovering shows the full detail. The rules live in
`cpp/link_health.cpp` and are unit-tested in `link_health_test`:

| Link | Red when |
| --- | --- |
| 裁判 | disconnected (`重连中`), or connected but nothing received for 2 s (`无数据`) |
| 车端 | no `CustomByteBlock` within `stale_window_ms`, or never one |
| 图传 | no decoded frame for 1 s |
| 控制器 | serial port missing or closed (`未接`), or no valid frame for 200 ms (`无数据`); amber `发不出` when frames arrive but MQTT is down |

The 2 s rule matters because a pulled cable does not close the TCP connection; the
connection only drops after the MQTT keepalive times out, about two keepalive
periods (`mqtt_keepalive_s`, default 5 s).

The self-check page (`自检` button) lists 席位 (未核对 until `RobotStaticStatus`
arrives, then 席位正确, or a red 席位选错 naming both seats), the four links, 录像
and 语音提醒,
each 正常 / 注意 / 异常 / 未启用, and says `可以上场` only when nothing is 异常. It
opens by itself when the match is not yet running (no match data, 赛外, 准备 or
自检), closes when the countdown starts, and stays closed for the rest of that
phase once the operator closes it. While it is open the ordinary popups give way.
Replay mode shows neither the lights nor the page.

The controller link (`cpp/controller_link.cpp`) reads the firmware's frames
(`0xA5` header with CRC8, command `0x0302`, 30 data bytes, CRC16; 115200 8N1, 25 Hz)
and forwards each valid frame once, as `CustomControl` (the robot receives it as
0x0311, at most 60 bytes in 2027), capped at 75 Hz. The other direction exists since
2027 dropped 0x0309: the hero puts its arm feedback (6 joint angles + flag, 25 bytes)
at the start of its 0x0310 data, the terminal takes those bytes from
`CustomByteBlock` and writes them to the controller as the 0x0309 frame the firmware
already expects, at most every 15 ms. It never re-sends a frame: when the cable is pulled or MQTT is
down, nothing is queued, so the robot never chases a stale pose. Reopening is
automatic every 500 ms, so a replugged cable recovers without restarting. On Linux
the user must be in the `dialout` group to open `/dev/ttyUSB*`; use a
`/dev/serial/by-id/...` path, which survives replugging. Forwarded frames are also
recorded.

## 事件提醒 (match alerts)

`cpp/match_alerts.cpp` turns each snapshot into new alerts, unit-tested in
`match_alerts_test`. Each alert shows a banner under the top bar (critical red,
warning amber, info cyan; 4 s each, critical first, anything that waited over 10 s
is dropped) and speaks a fixed phrase. A due tactical reminder also shows a banner,
whether or not the machine has a voice.

| `alerts_disabled` name | Fires when | How often |
| --- | --- | --- |
| `outpost` | our outpost zone becomes enemy temporary control; again when it becomes enemy full control | once per change |
| `control` | enemy control points lead by `alert_control_gap` | once; re-armed after the gap falls below half |
| `fortress` | the enemy starts activating our fortress (10 s of activation open our base armour) | once per activation |
| `dart` | 10 s before and at the dart gate chances (0:30, 4:00) | once per point |
| `energy` | small energy chances (0:00, 1:30), large (3:00, 4:15, 5:30) | once per point |
| `low_hp` | an enemy robot drops to 25% HP or below | once per robot until it recovers above 50% or dies |
| `base` | our base HP drops | at most once per 10 s, and the map flashes our half for 3 s |

The times are match elapsed seconds from the 2027 rules manual V1.0.0 (§5.6.3
energy, §5.7.3 dart). Timeline points only fire within 5 s of being crossed, so
joining mid-match or jumping the clock never replays old ones. Zones, control points
and base HP come from `GlobalUnitStatus`, the fortress timer from
`GlobalSpecialMechanism`.

Voice: alert phrases are synthesized before the match by a second speech backend
(`cpp/alert_voice.cpp`) and played through the reminder controller, which owns the
single speaker: an alert never interrupts a sentence, but goes ahead of the next
tactical reminder. macOS uses `say`. Linux uses Piper with the 超文 voice, installed
by `bash scripts/setup_voice.sh` (about 265 MB in `~/.local/share/rm_terminal/tts`,
model checked against its published SHA-256); `scripts/tts_say.py` converts text to
pinyin with pypinyin because Piper's own conversion misreads common words. Without
it the self-check shows 语音提醒 · 注意 and alerts are banner-only. Replays show
alert banners but stay silent.

`scripts/accept_alerts.py` drives the simulator through every alert (seek, speed,
zones, fortress, HP, structures) and asserts each fired the right number of times,
about 1 minute. It and `accept_links.py` point `speech_dir` at an empty directory
so a test run never talks through the speakers.

### Fault injection and acceptance

`run.sh` starts three extra simulator processes: `sim/link_proxy.py` (the terminal
reaches the broker through it on port 3335, so its link can be cut or frozen
without disturbing the simulator), `sim/fake_arm.py` (a pty at
`/tmp/rm_terminal_arm` sending firmware-format frames), and `video_sender` now
obeys a pause switch. The control panel's 链路 section, or the command line, flips
them:

```sh
.venv/bin/python -m sim.links mqtt cut|freeze|ok
.venv/bin/python -m sim.links car off|ok
.venv/bin/python -m sim.links video off|ok
.venv/bin/python -m sim.links arm unplug|plug|bad
.venv/bin/python -m sim.links reset
```

The simulated robot reports the arm pose it last received on `sim/ArmEcho` and
marks it stopped after 200 ms without a new one.

`scripts/accept_links.py` runs the whole table unattended (about two minutes, on a
private stack, refusing to start if 3333/3335 are busy): each link is cut and must
turn red within its limit while the others stay green, then recover by itself; the
arm must stop the robot within 1 s of unplugging; a corrupt frame must be logged
without dropping the link; all four at once must not crash the terminal. With
`-DRM_TERMINAL_INTEGRATION_TESTS=ON` it is also the CTest case `link_faults_e2e`.

## 席位与布局 (seats and role layouts)

The seat is fixed for a run and decides three things: the MQTT client id (the
protocol requires the connected robot's id: red 3 logs in as `3`, blue 3 as `103`),
red or blue for every 己方/对方 field, and the layout. Sources, highest first:
`--seat <id>`, `RM_SELF_ID`, an explicit `self_robot_id` in the config, then the seat
picker page (`qml/SeatPicker.qml`), which opens before the network starts; cancelling
it exits `0` with `seat_cancelled`. Replays use the seat stored in the recording.
`启动.command` asks for the seat in the terminal window (the simulator needs it
too) and exports `RM_SELF_ID`, so the picker does not open twice.

`cpp/layout_profile.cpp` holds one grid per role; the shared parts (status bar with
the seat label, top bar, alerts, popups, the video page) are the same for all three:

| Role | Seat | Info page |
| --- | --- | --- |
| 步兵 infantry | 3, 4 | ally roster / map / enemy roster; events / battle analysis / video |
| 重装 hero | 1 | the map spans two columns; events / battle analysis / 装配 / video |
| 指挥手 commander | 6 (the aerial seat, rules table 3-1) | both rosters stacked left, map over two rows with the mark toolbar, 飞镖 and 哨兵 right; events / battle analysis / video |

Role panes (`cpp/assembly_pane.*`, `cpp/dart_pane.*`, `cpp/sentry_pane.*`) show
`--` and name the message they wait for when data is missing. Their buttons send
`AssemblyCommand`, `DartCommand` and `SentryCtrlCommand` at most 10 times a second,
are disabled while MQTT is down (clicks are dropped, never queued), and need a 1 s
hold for 确认发射, 花金币复活 and 取消装配. Each send waits for the server's answer
(`SentryCtrlResult`, a `DartSelectTargetStatusSync` change, Event 15) and shows
未确认 after 2 s without one. The commander's map is clickable: open ground marks a
position, an enemy robot marks that robot (`MapClickCmd`, 0.5 s minimum interval,
custom letters C–L, N, O, Q–Z only). Every seat draws received `MapClickInfo` marks;
the commander also sees the sentry's planned path (`RobotPathPlanInfo`).

`RM_TERMINAL_CAPTURE_SIZE=1920x1080` sets the offscreen capture size (default
1440x900). `bash scripts/shoot_roles.sh` renders the three roles at 1440x900 and
1920x1080 against the simulator into `artifacts/roles/` (PNG + layout JSON with
`role`). `.venv/bin/python scripts/accept_roles.py` runs one match per role against
the simulator and checks, from the terminal log, the seat check, received marks,
dart gate and launch, sentry results and assembly results (about 2 minutes). The
offscreen terminal cannot be clicked, so the script publishes the commands the
buttons would send; the buttons themselves are covered by `role_panes_test`.

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

The simulator speaks to one seat, `RM_SELF_ID` (default `3`; the terminal must sit on
the same seat), and publishes the official messages from that seat's point of view.
`sim/test_protocol_2027.py` pins the seat-relative encodings and orderings; run it
for each seat, e.g. `RM_SELF_ID=106 .venv/bin/python -m sim.test_protocol_2027`.
`sim/State` is a simulator-only JSON snapshot in absolute red/blue terms for the
control panel; the terminal does not read it. `bash scripts/accept_sim_control.sh`
drives the simulator alone and checks the official messages it sends.

With `RM_BLIND_AUTO=1` the simulated enemy dart hits our base at deterministic
match seconds (`_BLIND_SCHEDULE`: 30 s and 150 s, random moving target), so two runs
are byte-identical:

```sh
RM_BLIND_AUTO=1 .venv/bin/python -u sim/match_server.py > /tmp/run1.log 2>&1
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

## Ubuntu

```sh
sudo apt-get install build-essential cmake \
  qt6-base-dev qt6-declarative-dev qt6-svg-dev \
  qml6-module-qtquick qml6-module-qtquick-layouts \
  libprotobuf-dev protobuf-compiler libabsl-dev \
  libmosquitto-dev mosquitto ffmpeg \
  python3-venv python3-tk fonts-noto-cjk
cmake -S . -B build/ubuntu -DCMAKE_BUILD_TYPE=Release
cmake --build build/ubuntu --parallel
ctest --test-dir build/ubuntu --output-on-failure
build/ubuntu/rm_terminal --safe-smoke
```

`qt6-base-dev` alone is not enough: `CMakeLists.txt` needs Qml/Quick/QuickWidgets
(`qt6-declarative-dev`), and the QML under `qml/` imports `QtQuick` and
`QtQuick.Layouts` at runtime, which come from the two `qml6-module-*` packages. `ffmpeg` decodes the video
feed, `python3-tk` is needed by `sim/control_panel.py`, and `fonts-noto-cjk`
supplies the Chinese glyphs.

`run.sh`, `启动.command`, `回放录像.command` and `scripts/verify_mode_regression.sh`
pick `build/ubuntu` on Linux and `build/macos` on macOS, so no `BUILD_DIR` is
needed. The `.command` files are plain bash; on Linux run them with
`bash 启动.command`. Recordings live in `~/.local/share/rm_terminal/recordings`
and `回放录像.command` lists them by number instead of the macOS file picker.

Voice on Linux needs `bash scripts/setup_voice.sh` once (see 事件提醒). The
`reminder_audio` test drives the real macOS `say`/`afplay` and is registered on
macOS only.

## 平台验证状态 (platform verification status)

| Platform | Build | CTest | Dynamic run (simulator, dual mode, video) |
| --- | --- | --- | --- |
| macOS (arm64, Homebrew Qt6) | verified | verified, Debug and Release | verified |
| Ubuntu 26.04 (x86_64, Qt 6.10.2, Wayland) | verified, Release | verified, 34/34 + `mode_regression_e2e` + `link_faults_e2e` | verified |

Ubuntu was verified on 2026-10-23 at commit `d1730fe`: Release build, the 25-test
default CTest suite, `--safe-smoke`, `python -m unittest` over `sim/`,
`scripts/verify_mode_regression.sh` (PASS, video entered at +33.09s, held 15.00s),
`bash 启动.command` with the control panel against a live simulator, offscreen
`--screenshot` in both modes with decoded video, and `--replay` of recordings
ended both by closing the window and by Ctrl-C. On 2026-10-26 the link lights,
self-check page and controller forwarding were verified on the same machine: the
28-test suite, `scripts/accept_links.py` (30/30) and offscreen screenshots with all
links up and with the video and arm cut. They have not been run on macOS yet.

On 2026-10-30 the 2027 protocol migration was verified on the same machine: the
34-test suite; the simulator's unit tests, with `test_protocol_2027` for all eight
seats; `scripts/accept_sim_control.sh` (all pass); `scripts/verify_mode_regression.sh`
(PASS, video entered at +33.31s, held 13.25s for the 10 s blind window plus 3 s);
`scripts/accept_links.py` (35/35); `scripts/accept_alerts.py` (18/18);
`scripts/accept_roles.py` (24/24); and `scripts/shoot_roles.sh` at 1440x900 and
1920x1080. None of this has been run on macOS, and nothing has been run against an
official server yet.

On 2026-10-31 the infantry/hero self commands (performance selection, exchange, remote
exchange, revive, energy rune) and keyboard/mouse control were added and verified on the
same machine: the 35-test suite (new `km_control`; new cases in `role_commands`,
`role_panes`, `intake_contract`, `dashboard_layout`); `sim/test_terminal_commands.py`
for seats 3 and 101 and `sim/test_respawn.py`; `scripts/accept_roles.py` (46/46),
`accept_links.py` (35/35), `accept_alerts.py` (18/18), `accept_sim_control.sh` and
`verify_mode_regression.sh` (all pass); `shoot_roles.sh`. The real binary was also driven
under Xvfb with `QT_QPA_PLATFORM=xcb` and XTest input: every new button clicked against
the simulator, and F9 + held W + 600 px of mouse motion + left click + one wheel notch,
recorded off the broker as 297 frames at 73 Hz summing to exactly +600 px, ending with an
all-released frame after Esc. The simulator's own robot now waits for 确认复活 after its
revive bar fills (protocol 2.2.25, item 3); other robots still revive on their own.

A recording cut off mid-record (crash, `kill -9`, power loss) still replays: the
player stops at the last complete record and the status bar says
`录像结尾不完整`.
The Debug build is not yet verified on Ubuntu. The timing numbers, screenshots,
and receipts in `artifacts/` and `.omo/evidence/` were produced on macOS.
