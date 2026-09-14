#!/usr/bin/env python3
"""Re-runnable evidence for the task-5 event classes that need a live UDP path.

lifecycle_harness.py proves startup, shutdown and readonly_block from the
terminal binary. The remaining classes only appear while real datagrams flow,
so this harness drives video_diagnostic --log against the Python sender and
asserts the emitted records. stale_data is covered deterministically by the
domain_state CTest test instead, which needs no sockets.

No GNU timeout on this macOS QA host, so every wait uses a monotonic deadline
and every child is reaped in a finally block.
"""

import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ASSET = os.path.join(ROOT, "assets", "sim_feed.h265")
SENDER = os.path.join(ROOT, "sim", "video_sender.py")
DIAGNOSTIC = os.path.join(ROOT, "build", "debug", "video_diagnostic")
TERMINAL = os.path.join(ROOT, "build", "debug", "rm_terminal")
RECORD = re.compile(
    r"^ts=(?P<ts>\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z) "
    r"level=(?P<level>debug|info|warning|error) "
    r"event=(?P<event>[a-z_]+)(?P<fields>(?: [a-z_]+=[^ ]*)*)$"
)

failures = []


def check(condition, label):
    print(f"  [{'ok' if condition else 'FAIL'}] {label}")
    if not condition:
        failures.append(label)


def reap(proc):
    if proc is None or proc.poll() is not None:
        return
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    except (ProcessLookupError, PermissionError):
        proc.terminate()
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline and proc.poll() is None:
        time.sleep(0.05)
    if proc.poll() is None:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            proc.kill()
        proc.wait(timeout=3)


def spawn_sender(port, loss):
    return subprocess.Popen(
        [sys.executable, SENDER, "--file", ASSET, "--host", "127.0.0.1",
         "--port", str(port), "--fps", "20", "--loss", str(loss)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)


def spawn_receiver(port, seconds, log, ffmpeg=None):
    cmd = [DIAGNOSTIC, "--bind", "127.0.0.1", "--port", str(port),
           "--duration", str(seconds), "--log", log]
    if ffmpeg:
        cmd += ["--ffmpeg", ffmpeg]
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, text=True, start_new_session=True)


def records(path):
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as handle:
        return [RECORD.match(line.strip()) for line in handle if line.strip()]


def scenario_decode_failure(work):
    print("marker=decode_failure")
    log = os.path.join(work, "decode.log")
    proc = spawn_receiver(39311, 3, log, ffmpeg="rm-nonexistent-ffmpeg")
    try:
        rc = proc.wait(timeout=30)
    finally:
        reap(proc)
    parsed = records(log)
    check(all(m is not None for m in parsed), "every decode record matches the grammar")
    hits = [m for m in parsed if m and m.group("event") == "decode_failure"]
    check(rc == 3, "missing ffmpeg exits 3")
    check(len(hits) >= 1, "missing ffmpeg emits decode_failure")
    if hits:
        check(hits[0].group("level") == "error", "decode_failure is logged at error level")
        check("reason=ffmpeg_not_found" in hits[0].group("fields"),
              "decode_failure names the reason")
    print(f"marker=decode_failure records={len(hits)} rc={rc}")


def scenario_packet_loss(work):
    print("marker=packet_loss")
    log = os.path.join(work, "loss.log")
    seconds = 6
    receiver = spawn_receiver(39312, seconds, log)
    sender = None
    try:
        time.sleep(0.4)
        sender = spawn_sender(39312, 0.10)
        receiver.wait(timeout=seconds + 25)
    finally:
        reap(sender)
        reap(receiver)
    parsed = records(log)
    check(all(m is not None for m in parsed), "every loss record matches the grammar")
    hits = [m for m in parsed if m and m.group("event") == "packet_loss"]
    check(len(hits) >= 1, "10 percent loss emits packet_loss")
    # The emitter samples at most once per second. Without that cap a lossy
    # 20fps stream would emit thousands of records and bury every other event.
    check(len(hits) <= seconds + 1, f"packet_loss is rate limited ({len(hits)} in {seconds}s)")
    if hits:
        check(hits[0].group("level") == "warning", "packet_loss is logged at warning level")
        check("missing_total=" in hits[0].group("fields"),
              "packet_loss reports a cumulative counter")
    print(f"marker=packet_loss records={len(hits)} window_s={seconds}")


def scenario_reconnect_udp(work):
    print("marker=reconnect_udp")
    log = os.path.join(work, "reconnect.log")
    receiver = spawn_receiver(39313, 8, log)
    sender = None
    try:
        time.sleep(0.4)
        sender = spawn_sender(39313, 0.0)
        time.sleep(2.5)
        reap(sender)
        sender = None
        receiver.wait(timeout=40)
    finally:
        reap(sender)
        reap(receiver)
    parsed = records(log)
    check(all(m is not None for m in parsed), "every reconnect record matches the grammar")
    hits = [m for m in parsed if m and m.group("event") == "reconnect_udp"]
    check(len(hits) >= 1, "sender stop emits reconnect_udp")
    if hits:
        check("state=disconnected" in hits[0].group("fields"),
              "reconnect_udp reports the disconnected state")
        check("silent_ms=" in hits[0].group("fields"),
              "reconnect_udp reports the silence duration")
    print(f"marker=reconnect_udp records={len(hits)}")


def scenario_reconnect_mqtt(work):
    print("marker=reconnect_mqtt")
    log = os.path.join(work, "mqtt.log")
    conf = os.path.join(work, "mqtt.conf")
    with open(conf, "w", encoding="utf-8") as handle:
        handle.write(f"log_destination={log}\nlog_level=info\n")
    # Port 39399 has no broker, so intake.start() fails and the composition root
    # records the unreachable transport instead of silently continuing.
    proc = subprocess.Popen(
        [TERMINAL, "--config", conf, "--diagnostic", "127.0.0.1", "39399", "1"],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
        start_new_session=True)
    try:
        rc = proc.wait(timeout=30)
    finally:
        reap(proc)
    parsed = records(log)
    check(all(m is not None for m in parsed), "every mqtt record matches the grammar")
    names = [m.group("event") for m in parsed if m]
    hits = [m for m in parsed if m and m.group("event") == "reconnect_mqtt"]
    check(rc == 3, "unreachable broker exits 3")
    check(len(hits) == 1, "unreachable broker emits reconnect_mqtt")
    if hits:
        check(hits[0].group("level") == "error", "reconnect_mqtt is logged at error level")
        check("host=127.0.0.1" in hits[0].group("fields"), "reconnect_mqtt names the host")
    check(names and names[-1] == "shutdown", "a failed transport still logs shutdown")
    print(f"marker=reconnect_mqtt records={len(hits)} rc={rc}")


def main():
    started = time.monotonic()
    for path, label in ((DIAGNOSTIC, "missing_diagnostic"), (TERMINAL, "missing_terminal"),
                        (ASSET, "missing_asset"), (SENDER, "missing_sender")):
        if not os.path.exists(path):
            print(f"marker=abort reason={label} path={path}")
            return 1

    work = tempfile.mkdtemp(prefix="rm-task5-events-")
    try:
        scenario_decode_failure(work)
        scenario_packet_loss(work)
        scenario_reconnect_udp(work)
        scenario_reconnect_mqtt(work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
        leaked = subprocess.run(
            ["pgrep", "-f", "ffmpeg|video_sender|video_diagnostic"],
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
