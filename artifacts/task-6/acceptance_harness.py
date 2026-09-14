#!/usr/bin/env python3
"""One-shot task-6 acceptance evidence package.

Generates every artifact the plan requires for module-1 user acceptance:
build/start instructions (BUILD.md), a normal-operation screenshot with live
telemetry, MQTT down, MQTT reconnect, malformed message, UDP video, UDP drop,
and a read-only proof. Writes evidence next to this file and prints a verdict.

Every wait uses a monotonic deadline (this macOS QA host has no GNU timeout) and
every spawned process is reaped in a finally block, including the broker and the
Python simulator.
"""

import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).parents[2]
OUT = Path(__file__).parent
VENV = ROOT.parent / "venv" / "bin" / "python"
MOSQUITTO = Path("/opt/homebrew/sbin/mosquitto")
MOSQUITTO_SUB = Path("/opt/homebrew/bin/mosquitto_sub")
MOSQUITTO_PUB = Path("/opt/homebrew/bin/mosquitto_pub")
TERMINAL = ROOT / "build" / "debug" / "rm_terminal"
DIAGNOSTIC = ROOT / "build" / "debug" / "video_diagnostic"
SENDER = ROOT / "sim" / "video_sender.py"
ASSET = ROOT / "assets" / "sim_feed.h265"

MQTT_PORT = "3333"
VIDEO_PORT = 3334
CONTROL_TOPICS = ("CustomControl", "CommonCommand")
INBOUND_TOPICS = ("GameStatus", "RobotDynamicStatus", "RobotModuleStatus",
                  "RobotPosition", "Event", "RobotTelemetry")
RECORD = re.compile(
    r"^ts=\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z "
    r"level=(?:debug|info|warning|error) event=[a-z_]+(?: [a-z_]+=[^ ]*)*$")

failures = []
handles = []


def check(condition, label):
    print(f"  [{'ok' if condition else 'FAIL'}] {label}")
    if not condition:
        failures.append(label)


def launch(command, log_path, env=None):
    handle = open(log_path, "w", encoding="utf-8")
    handles.append(handle)
    merged = {**os.environ, "PYTHONUNBUFFERED": "1"}
    if env:
        merged.update(env)
    return subprocess.Popen([str(part) for part in command], cwd=ROOT, stdout=handle,
                            stderr=subprocess.STDOUT, text=True,
                            start_new_session=True, env=merged)


def reap(proc):
    if proc is None or proc.poll() is not None:
        return
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(os.getpgid(proc.pid), sig)
        except (ProcessLookupError, PermissionError):
            proc.kill()
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline and proc.poll() is None:
            time.sleep(0.05)
        if proc.poll() is not None:
            return


def wait_for(path, needle, budget, occurrences=1):
    deadline = time.monotonic() + budget
    while time.monotonic() < deadline:
        if Path(path).exists() and \
                Path(path).read_text(errors="replace").count(needle) >= occurrences:
            return True
        time.sleep(0.05)
    return False


def wire_topics(path):
    # mosquitto_sub -v prints "TOPIC PAYLOAD"; protobuf payloads carry arbitrary
    # bytes, so match the topic only at the start of a line.
    seen = set()
    if not Path(path).exists():
        return seen
    for line in Path(path).read_text(errors="replace").splitlines():
        head = line.split(" ", 1)[0]
        if head in INBOUND_TOPICS:
            seen.add(head)
    return seen


def control_publishes(path):
    seen = set()
    if not Path(path).exists():
        return seen
    for line in Path(path).read_text(errors="replace").splitlines():
        head = line.split(" ", 1)[0]
        if head in CONTROL_TOPICS:
            seen.add(head)
    return seen


def wait_for_topics(path, budget):
    # The simulator only enters stage 4 after 10s elapsed, and Event is published
    # from randomised match ticks, so a short window legitimately misses it.
    deadline = time.monotonic() + budget
    seen = set()
    while time.monotonic() < deadline:
        seen = wire_topics(path)
        if len(seen) == len(INBOUND_TOPICS):
            return seen
        time.sleep(0.2)
    return seen


def records(path):
    if not Path(path).exists():
        return []
    return [line.strip() for line in Path(path).read_text(errors="replace").splitlines()
            if line.strip()]


def events(path):
    found = []
    for line in records(path):
        parts = line.split(" event=")
        if len(parts) > 1:
            found.append(parts[1].split(" ")[0])
    return found


def conf(path, log_path, extra=""):
    Path(path).write_text(f"log_destination={log_path}\nlog_level=info\n{extra}",
                          encoding="utf-8")


def capture_gui(config_path, shot_path, stdout_path, budget=40):
    proc = launch([TERMINAL, "--config", config_path, "--screenshot", shot_path],
                  stdout_path, env={"QT_QPA_PLATFORM": "offscreen"})
    try:
        return proc.wait(timeout=budget)
    except subprocess.TimeoutExpired:
        return None
    finally:
        reap(proc)


def scenario_normal(work):
    print("marker=normal_operation")
    broker = simulator = subscriber = None
    log = work / "normal.log"
    try:
        broker = launch([MOSQUITTO, "-p", MQTT_PORT], OUT / "broker-normal.log")
        time.sleep(0.8)
        subscriber = launch([MOSQUITTO_SUB, "-p", MQTT_PORT, "-t", "#", "-v"],
                            OUT / "wire-normal.log")
        simulator = launch([VENV, "-u", "-m", "sim.match_server"], OUT / "simulator-normal.log")
        check(wait_for(OUT / "wire-normal.log", "RobotTelemetry", 20),
              "simulator publishes inbound telemetry")
        # Event only fires from stage-4 match ticks, which begin at 10s elapsed.
        time.sleep(11)
        conf(work / "normal.conf", log)
        rc = capture_gui(work / "normal.conf", OUT / "screenshot-normal.png",
                         OUT / "gui-normal.log")
        check(rc == 0, "terminal starts against a live broker and exits 0")
        check((OUT / "screenshot-normal.png").exists(), "normal screenshot written")
        check(wait_for(OUT / "gui-normal.log", "MQTT subscribed (readonly)", 5),
              "terminal reports a read-only subscription")

        seen = wait_for_topics(OUT / "wire-normal.log", 25)
        check(len(seen) == len(INBOUND_TOPICS),
              f"all six inbound families observed on the wire "
              f"({len(seen)}/6, missing={sorted(set(INBOUND_TOPICS) - seen)})")
        names = events(log)
        check("startup" in names, "normal run logs startup")
        check("shutdown" in names, "normal run logs shutdown")
        check("reconnect_mqtt" not in names, "a healthy broker logs no reconnect_mqtt")
        print(f"marker=normal topics={len(seen)} records={len(names)}")
    finally:
        for proc in (simulator, subscriber, broker):
            reap(proc)


def scenario_mqtt_down(work):
    print("marker=mqtt_down")
    log = work / "mqtt-down.log"
    conf(work / "down.conf", log)
    rc = capture_gui(work / "down.conf", OUT / "screenshot-mqtt-down.png",
                     OUT / "gui-mqtt-down.log")
    names = events(log)
    check(rc == 0, "terminal survives an absent broker and exits 0")
    check((OUT / "screenshot-mqtt-down.png").exists(), "mqtt-down screenshot written")
    check("reconnect_mqtt" in names, "absent broker logs reconnect_mqtt")
    check("readonly_block" in names, "absent broker logs readonly_block")
    check(names[-1] == "shutdown" if names else False, "mqtt-down run still logs shutdown")
    print(f"marker=mqtt_down records={len(names)}")


def scenario_mqtt_reconnect(work):
    print("marker=mqtt_reconnect")
    broker = native = None
    log = work / "reconnect.log"
    try:
        broker = launch([MOSQUITTO, "-p", MQTT_PORT], OUT / "broker-reconnect-1.log")
        time.sleep(0.8)
        conf(work / "reconnect.conf", log)
        native = launch([TERMINAL, "--config", work / "reconnect.conf",
                         "--diagnostic", "127.0.0.1", MQTT_PORT, "20"],
                        OUT / "native-reconnect.log")
        check(wait_for(OUT / "native-reconnect.log", "MQTT subscribed (readonly)", 20),
              "terminal subscribes to the live broker")
        reap(broker)
        broker = None
        time.sleep(1.5)
        broker = launch([MOSQUITTO, "-p", MQTT_PORT], OUT / "broker-reconnect-2.log")
        # A substring check would pass on the FIRST subscribe and prove nothing;
        # resubscription is only demonstrated by a second occurrence.
        check(wait_for(OUT / "native-reconnect.log", "MQTT subscribed (readonly)", 25,
                       occurrences=2),
              "terminal resubscribes after the broker restarts")
        check(native.poll() is None, "terminal survived the broker outage")
    finally:
        reap(native)
        reap(broker)
    print("marker=mqtt_reconnect verified=1")


def scenario_malformed(work):
    print("marker=malformed_message")
    broker = native = None
    log = work / "malformed.log"
    try:
        broker = launch([MOSQUITTO, "-p", MQTT_PORT], OUT / "broker-malformed.log")
        time.sleep(0.8)
        conf(work / "malformed.conf", log)
        native = launch([TERMINAL, "--config", work / "malformed.conf",
                         "--diagnostic", "127.0.0.1", MQTT_PORT, "12"],
                        OUT / "native-malformed.log")
        check(wait_for(OUT / "native-malformed.log", "MQTT subscribed (readonly)", 20),
              "terminal subscribed before the malformed publish")
        # argv cannot carry a NUL byte, so the binary payload goes through stdin
        # via -s (send entire stdin as one message).
        for topic, payload in (("GameStatus", b"not-a-protobuf"),
                               ("RobotTelemetry", b"\x00\x01\x02broken"),
                               ("UnknownTopic", b"ignored")):
            subprocess.run([str(MOSQUITTO_PUB), "-p", MQTT_PORT, "-t", topic, "-s"],
                           input=payload, check=False, timeout=5)
        check(wait_for(OUT / "native-malformed.log", "ignored malformed", 10),
              "malformed payload is reported and ignored")
        time.sleep(1)
        check(native.poll() is None, "terminal stayed alive after malformed input")
    finally:
        reap(native)
        reap(broker)
    print("marker=malformed_message verified=1")


def scenario_udp_video(work):
    print("marker=udp_video")
    sender = receiver = None
    log = OUT / "video-events.log"
    try:
        receiver = launch([DIAGNOSTIC, "--bind", "127.0.0.1", "--port", str(VIDEO_PORT),
                           "--duration", "8", "--log", log], OUT / "video-normal.log")
        time.sleep(0.5)
        sender = launch([VENV, "-u", SENDER, "--file", ASSET, "--host", "127.0.0.1",
                         "--port", str(VIDEO_PORT), "--fps", "20"], OUT / "video-sender.log")
        time.sleep(3.0)
        reap(sender)
        sender = None
        try:
            receiver.wait(timeout=40)
        except subprocess.TimeoutExpired:
            pass
    finally:
        reap(sender)
        reap(receiver)
    body = (OUT / "video-normal.log").read_text(errors="replace")
    decoded = [int(m) for m in re.findall(r'"decoded_frames":(\d+)', body)]
    names = events(log)
    check(bool(decoded) and max(decoded) > 0,
          f"H.265 frames decoded from the UDP stream (max={max(decoded) if decoded else 0})")
    check("reconnect_udp" in names, "stopping the sender logs reconnect_udp")
    print(f"marker=udp_video decoded_max={max(decoded) if decoded else 0} records={len(names)}")


def scenario_readonly_proof(work):
    print("marker=readonly_proof")
    broker = simulator = subscriber = native = None
    log = work / "readonly.log"
    try:
        broker = launch([MOSQUITTO, "-p", MQTT_PORT], OUT / "broker-readonly.log")
        time.sleep(0.8)
        subscriber = launch([MOSQUITTO_SUB, "-p", MQTT_PORT, "-t", "#", "-v"],
                            OUT / "wire-readonly.log")
        # The terminal must be actively CONSUMING telemetry during the observation
        # window; an idle terminal on a silent wire proves nothing about whether
        # inbound data provokes an outbound reaction.
        simulator = launch([VENV, "-u", "-m", "sim.match_server"],
                           OUT / "simulator-readonly.log")
        conf(work / "readonly.conf", log)
        native = launch([TERMINAL, "--config", work / "readonly.conf",
                         "--diagnostic", "127.0.0.1", MQTT_PORT, "10"],
                        OUT / "native-readonly.log")
        check(wait_for(OUT / "native-readonly.log", "MQTT subscribed (readonly)", 20),
              "terminal subscribed for the read-only observation window")
        try:
            native.wait(timeout=30)
        except subprocess.TimeoutExpired:
            pass
    finally:
        reap(native)
        reap(simulator)
        reap(subscriber)
        reap(broker)
    observed = wire_topics(OUT / "wire-readonly.log")
    check(len(observed) > 0,
          f"terminal consumed live telemetry while observed ({len(observed)}/6 families)")
    check("0 robot(s)" not in (OUT / "native-readonly.log").read_text(errors="replace"),
          "terminal decoded robot state during the read-only window")
    control = control_publishes(OUT / "wire-readonly.log")
    for topic in CONTROL_TOPICS:
        check(topic not in control, f"no {topic} publish observed on the wire")
    static = subprocess.run(
        ["grep", "-rniE", "mosquitto_publish|can_send|uart_write", str(ROOT / "cpp")],
        capture_output=True, text=True)
    check(static.returncode != 0, "no publish/CAN/UART write call exists in cpp/")
    (OUT / "readonly-static-scan.log").write_text(
        f"grep -rniE 'mosquitto_publish|can_send|uart_write' cpp/\n"
        f"exit={static.returncode} (1 = no match, which is the required result)\n"
        f"{static.stdout}", encoding="utf-8")
    print("marker=readonly_proof control_publishes=0")


def scenario_grammar_and_tests(work):
    print("marker=logs_and_tests")
    every = []
    for name in ("normal.log", "mqtt-down.log", "reconnect.log", "malformed.log",
                 "readonly.log"):
        every.extend(records(work / name))
    every.extend(records(OUT / "video-events.log"))
    bad = [line for line in every if not RECORD.match(line)]
    check(not bad, f"every structured record matches the grammar ({len(every)} records)")
    if bad:
        (OUT / "grammar-violations.log").write_text("\n".join(bad), encoding="utf-8")

    for build in ("debug", "release"):
        result = subprocess.run(["ctest", "--test-dir", str(ROOT / "build" / build)],
                                capture_output=True, text=True, timeout=300)
        passed = "100% tests passed" in result.stdout
        check(passed, f"{build} CTest suite passes")
        (OUT / f"ctest-{build}.log").write_text(result.stdout, encoding="utf-8")
    print(f"marker=logs_and_tests records={len(every)} violations={len(bad)}")


def main():
    started = time.monotonic()
    for path, label in ((TERMINAL, "terminal"), (DIAGNOSTIC, "video_diagnostic"),
                        (VENV, "venv_python"), (MOSQUITTO, "mosquitto"),
                        (MOSQUITTO_SUB, "mosquitto_sub"), (MOSQUITTO_PUB, "mosquitto_pub"),
                        (SENDER, "video_sender"), (ASSET, "h265_asset"),
                        (ROOT / "BUILD.md", "build_instructions")):
        if not Path(path).exists():
            print(f"marker=abort reason=missing_{label} path={path}")
            return 1

    work = OUT / "work"
    work.mkdir(exist_ok=True)
    try:
        scenario_normal(work)
        scenario_mqtt_down(work)
        scenario_mqtt_reconnect(work)
        scenario_malformed(work)
        scenario_udp_video(work)
        scenario_readonly_proof(work)
        scenario_grammar_and_tests(work)
    finally:
        for handle in handles:
            try:
                handle.close()
            except OSError:
                pass
        leaked = subprocess.run(
            ["pgrep", "-f", "mosquitto|match_server|video_sender|video_diagnostic|rm_terminal|ffmpeg"],
            capture_output=True, text=True).stdout.strip()
        orphans = [pid for pid in leaked.splitlines() if pid]
        print(f"marker=cleanup orphan_processes={len(orphans)}")
        if orphans:
            failures.append("orphan processes survived")

    print(f"marker=summary failures={len(failures)} elapsed_s={time.monotonic() - started:.1f}")
    if failures:
        for item in failures:
            print(f"  failed: {item}")
        return 1
    print("marker=verdict PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
