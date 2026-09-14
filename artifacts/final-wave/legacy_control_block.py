#!/usr/bin/env python3
"""Proves the legacy Python terminal can no longer emit control commands.

Static absence of `.publish(` is necessary but not sufficient: the UI buttons and
keyboard shortcuts still CALL send_custom_control/send_common_command. This runs
a real broker, invokes every control entry point the UI is wired to, and asserts
a wildcard subscriber observes zero control messages.
"""

import os
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
PORT = "3333"

DRIVER = r'''
import sys, time
sys.path.insert(0, ".")
sys.path.insert(0, "generated")
from core.mqtt_link import MqttLink
from core.constants import (CMD_EMERGENCY_STOP, CMD_RESET_GIMBAL,
                            CMD_TOGGLE_AUTOAIM, CMD_SET_CHASSIS_MODE)
link = MqttLink(host="127.0.0.1", port=3333)
link.start()
time.sleep(2.0)
results = []
for name, cid in (("EMERGENCY_STOP", CMD_EMERGENCY_STOP),
                  ("RESET_GIMBAL", CMD_RESET_GIMBAL),
                  ("TOGGLE_AUTOAIM", CMD_TOGGLE_AUTOAIM),
                  ("SET_CHASSIS_MODE", CMD_SET_CHASSIS_MODE)):
    results.append((name, link.send_custom_control(cid, 1)))
results.append(("COMMON_COMMAND", link.send_common_command(99, 1)))
time.sleep(1.5)
link.stop()
for name, ok in results:
    print(f"DRIVER {name} returned={ok}")
print("DRIVER any_true=", any(ok for _, ok in results))
'''

failures = []


def check(condition, label):
    print(f"  [{'ok' if condition else 'FAIL'}] {label}")
    if not condition:
        failures.append(label)


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


def main():
    print("marker=legacy_control_block")
    broker = subscriber = None
    wire = OUT / "legacy-wire.log"
    try:
        with open(OUT / "legacy-broker.log", "w") as bl:
            broker = subprocess.Popen([str(MOSQUITTO), "-p", PORT], stdout=bl,
                                      stderr=subprocess.STDOUT, start_new_session=True)
        time.sleep(0.8)
        with open(wire, "w") as wl:
            subscriber = subprocess.Popen([str(MOSQUITTO_SUB), "-p", PORT, "-t", "#", "-v"],
                                          stdout=wl, stderr=subprocess.STDOUT,
                                          start_new_session=True)
        time.sleep(0.5)
        driver = subprocess.run([str(VENV), "-u", "-c", DRIVER], cwd=ROOT,
                                capture_output=True, text=True, timeout=120)
        body = driver.stdout + driver.stderr
        (OUT / "legacy-driver.log").write_text(body, encoding="utf-8")
        time.sleep(1.0)
    finally:
        reap(subscriber)
        reap(broker)

    check(driver.returncode == 0, f"driver exercised every control entry point (rc={driver.returncode})")
    check("DRIVER any_true= False" in body, "every control call returned False (blocked)")
    check(body.count("[readonly_block]") == 5,
          f"each blocked attempt was reported ({body.count('[readonly_block]')}/5)")

    observed = set()
    for line in wire.read_text(errors="replace").splitlines():
        head = line.split(" ", 1)[0]
        if head in ("CustomControl", "CommonCommand"):
            observed.add(head)
    check(not observed, f"zero control messages on the wire (saw {sorted(observed) or 'none'})")

    print(f"marker=legacy_summary failures={len(failures)}")
    if failures:
        for f in failures:
            print(f"  failed: {f}")
        return 1
    print("marker=legacy_verdict PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
