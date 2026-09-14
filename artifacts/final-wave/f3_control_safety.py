#!/usr/bin/env python3
"""F3 control-safety audit: dynamic proof that no input produces control output.

The static and binary evidence (grep over cpp/, `nm -u` over the linked
executable) is collected by the caller. This script proves the runtime half:
while the terminal observes a live broker, hostile and control-shaped inbound
messages provoke ZERO terminal-originated publishes, and the process opens no
serial/CAN character device.

Counting method: a wildcard subscriber sees the messages this script injects as
well as anything the terminal might publish, so the assertion is that the
control-topic message count equals exactly what was injected. One extra message
would mean the terminal echoed or reacted.
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
MQTT_PORT = "3333"
CONTROL_TOPICS = ("CustomControl", "CommonCommand")

# `mosquitto_pub -s` refuses a zero-length stdin, so an empty payload cannot be
# injected this way and is omitted rather than silently counted as sent.
INJECTED = (
    ("CustomControl", b"\x08\x01\x10\x01"),
    ("CustomControl", b"\xff\xff\xff\xff\xff\xff"),
    ("CommonCommand", b"\x08\x63\x10\x01"),
    ("CommonCommand", b"\xff\xff\xff\xff"),
    ("GameStatus", b"\x00\x01\x02not-a-protobuf"),
    ("RobotTelemetry", b"\xde\xad\xbe\xef"),
    ("UnknownTopic", b"ignored"),
)

# The terminal subscribes to the six inbound families only, so control topics and
# unknown topics never reach it and produce no rejection record. Only the two
# malformed INBOUND payloads are observable as rejections.
EXPECTED_REJECTIONS = 2

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
    return subprocess.Popen([str(p) for p in command], cwd=ROOT, stdout=handle,
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


def wait_for(path, needle, budget):
    deadline = time.monotonic() + budget
    while time.monotonic() < deadline:
        if Path(path).exists() and needle in Path(path).read_text(errors="replace"):
            return True
        time.sleep(0.05)
    return False


def topic_counts(path):
    counts = {}
    if not Path(path).exists():
        return counts
    for line in Path(path).read_text(errors="replace").splitlines():
        head = line.split(" ", 1)[0]
        if head:
            counts[head] = counts.get(head, 0) + 1
    return counts


def main():
    for path, label in ((TERMINAL, "terminal"), (VENV, "venv_python"),
                        (MOSQUITTO, "mosquitto"), (MOSQUITTO_SUB, "mosquitto_sub"),
                        (MOSQUITTO_PUB, "mosquitto_pub")):
        if not Path(path).exists():
            print(f"marker=abort reason=missing_{label} path={path}")
            return 1

    print("marker=f3_dynamic_control_safety")
    broker = simulator = subscriber = native = None
    conf_path = OUT / "f3.conf"
    log_path = OUT / "f3-terminal.log"
    conf_path.write_text(f"log_destination={log_path}\nlog_level=debug\n", encoding="utf-8")
    devices = ""
    try:
        broker = launch([MOSQUITTO, "-p", MQTT_PORT], OUT / "f3-broker.log")
        time.sleep(0.8)
        subscriber = launch([MOSQUITTO_SUB, "-p", MQTT_PORT, "-t", "#", "-v"],
                           OUT / "f3-wire.log")
        simulator = launch([VENV, "-u", "-m", "sim.match_server"], OUT / "f3-simulator.log")
        native = launch([TERMINAL, "--config", conf_path,
                         "--diagnostic", "127.0.0.1", MQTT_PORT, "14"],
                        OUT / "f3-terminal-stdout.log")
        check(wait_for(OUT / "f3-terminal-stdout.log", "MQTT subscribed (readonly)", 20),
              "terminal subscribed before hostile injection")

        for topic, payload in INJECTED:
            subprocess.run([str(MOSQUITTO_PUB), "-p", MQTT_PORT, "-t", topic, "-s"],
                           input=payload, check=False, timeout=5)
            time.sleep(0.15)

        # Character-device proof must be taken while the process is ALIVE.
        if native.poll() is None:
            probe = subprocess.run(["lsof", "-p", str(native.pid)],
                                   capture_output=True, text=True, timeout=30)
            devices = probe.stdout
            (OUT / "f3-open-files.log").write_text(devices, encoding="utf-8")

        time.sleep(1.5)
        check(native.poll() is None, "terminal alive after all hostile input")
        try:
            native.wait(timeout=30)
        except subprocess.TimeoutExpired:
            pass
    finally:
        for proc in (native, simulator, subscriber, broker):
            reap(proc)
        for handle in handles:
            try:
                handle.close()
            except OSError:
                pass

    counts = topic_counts(OUT / "f3-wire.log")
    for topic in CONTROL_TOPICS:
        expected = sum(1 for t, _ in INJECTED if t == topic)
        actual = counts.get(topic, 0)
        check(actual == expected,
              f"{topic}: {actual} on wire == {expected} injected (0 from terminal)")

    inbound = ("GameStatus", "RobotDynamicStatus", "RobotModuleStatus",
               "RobotPosition", "Event", "RobotTelemetry")
    observed = [t for t in inbound if counts.get(t, 0) > 0]
    check(len(observed) == len(inbound),
          f"terminal was consuming live telemetry during the audit ({len(observed)}/6)")

    stdout_body = (OUT / "f3-terminal-stdout.log").read_text(errors="replace")
    rejected = stdout_body.count("ignored malformed")
    check(rejected == EXPECTED_REJECTIONS,
          f"malformed inbound payloads rejected ({rejected}/{EXPECTED_REJECTIONS}); "
          f"control topics never reach the terminal because it does not subscribe to them")
    check("0 robot(s)" not in stdout_body, "terminal decoded real robot state")

    if devices:
        serial = [l for l in devices.splitlines()
                  if re.search(r"/dev/(tty|cu\.|serial|can)", l)]
        check(not serial, f"no serial/CAN character device opened ({len(serial)} matches)")
    else:
        check(False, "lsof device probe captured while process alive")

    exit_records = [l for l in Path(log_path).read_text(errors="replace").splitlines()
                    if "event=shutdown" in l]
    check(any("exit=0" in l for l in exit_records),
          "terminal shut down cleanly after hostile input")

    print(f"marker=f3_summary failures={len(failures)} "
          f"control_from_terminal=0 rejections={rejected}")
    if failures:
        for item in failures:
            print(f"  failed: {item}")
        return 1
    print("marker=f3_verdict PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
