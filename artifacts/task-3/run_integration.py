from __future__ import annotations

import os
import signal
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).parents[2]
ARTIFACTS = Path(__file__).parent
PORT = "3333"
TOPICS = {
    "GameStatus",
    "RobotDynamicStatus",
    "RobotModuleStatus",
    "RobotPosition",
    "Event",
    "RobotTelemetry",
}


def launch(command: list[str], output: Path) -> subprocess.Popen[str]:
    handle = output.open("w", encoding="utf-8")
    process = subprocess.Popen(
        command,
        cwd=ROOT,
        stdout=handle,
        stderr=subprocess.STDOUT,
        text=True,
        start_new_session=True,
        env={**os.environ, "PYTHONUNBUFFERED": "1"},
    )
    process._task3_log = handle  # type: ignore[attr-defined]
    return process


def stop(process: subprocess.Popen[str] | None) -> None:
    if process is None:
        return
    try:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except PermissionError:
            process.terminate()
        process.wait(timeout=3)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        try:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except PermissionError:
                process.kill()
            except ProcessLookupError:
                pass
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)
    finally:
        process._task3_log.close()  # type: ignore[attr-defined]


def wait_for(path: Path, text: str, deadline: float) -> None:
    while time.monotonic() < deadline:
        if path.exists() and text.encode() in path.read_bytes():
            return
        time.sleep(0.05)
    raise RuntimeError(f"deadline waiting for {text!r} in {path}")


def main() -> int:
    broker = server = native = subscriber = None
    try:
        broker = launch(["/opt/homebrew/sbin/mosquitto", "-p", PORT], ARTIFACTS / "broker-live.log")
        time.sleep(0.5)
        subscriber = launch(["/opt/homebrew/bin/mosquitto_sub", "-p", PORT, "-t", "#", "-v"], ARTIFACTS / "wire-live.log")
        server = launch([str(ROOT / "../venv/bin/python"), "-u", "-m", "sim.match_server"], ARTIFACTS / "simulator-live.log")
        native = launch([str(ROOT / "build/task3-debug-fresh/rm_terminal"), "--diagnostic", "127.0.0.1", PORT, "12"], ARTIFACTS / "native-live.log")
        wait_for(ARTIFACTS / "native-live.log", "MQTT subscribed (readonly)", time.monotonic() + 8)
        wait_for(ARTIFACTS / "wire-live.log", "RobotTelemetry", time.monotonic() + 8)
        time.sleep(1)
        wire = (ARTIFACTS / "wire-live.log").read_bytes()
        seen = {topic for topic in TOPICS if (topic + " ").encode() in wire}
        missing = TOPICS - seen
        if missing:
            raise RuntimeError(f"missing live topics: {sorted(missing)}")
        subprocess.run(["/opt/homebrew/bin/mosquitto_pub", "-p", PORT, "-t", "GameStatus", "-m", "bad"], check=True, timeout=2)
        native_log = ARTIFACTS / "native-live.log"
        wait_for(native_log, "ignored malformed", time.monotonic() + 3)
        stop(broker)
        broker = None
        time.sleep(1)
        broker = launch(["/opt/homebrew/sbin/mosquitto", "-p", PORT], ARTIFACTS / "broker-reconnect.log")
        wait_for(native_log, "MQTT subscribed (readonly)", time.monotonic() + 8)
        time.sleep(1)
        wire = (ARTIFACTS / "wire-live.log").read_bytes()
        if b"CustomControl " in wire or b"CommonCommand " in wire:
            raise RuntimeError("native/control publish observed")
        print("six_topics=PASS")
        print("malformed_recovery=PASS")
        print("reconnect_recovery=PASS")
        print("native_publish=0")
        return 0
    finally:
        for process in (native, server, subscriber, broker):
            stop(process)


if __name__ == "__main__":
    raise SystemExit(main())
