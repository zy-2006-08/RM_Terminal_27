#!/usr/bin/env python3
"""Bounded sender stop/restart harness for the read-only video path.

Proves decoded_frames strictly increases after a sender restart. The macOS QA
host has no GNU timeout, so every wait uses a monotonic deadline and every
child is reaped in a finally block.
"""
import json
import os
import signal
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DIAG = os.path.join(ROOT, "build", "debug", "video_diagnostic")
ASSET = os.path.join(ROOT, "assets", "sim_feed.h265")
SENDER = os.path.join(ROOT, "sim", "video_sender.py")
PORT = "39176"


def spawn_sender(loss, jitter):
    return subprocess.Popen(
        [sys.executable, SENDER, "--file", ASSET, "--host", "127.0.0.1",
         "--port", PORT, "--fps", "20", "--loss", str(loss),
         "--jitter-ms", str(jitter)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        start_new_session=True)


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


def main():
    receiver = None
    sender = None
    snapshots = []
    try:
        receiver = subprocess.Popen(
            [DIAG, "--bind", "127.0.0.1", "--port", PORT, "--duration", "24"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, start_new_session=True)

        print("marker=phase_1_normal_play", flush=True)
        sender = spawn_sender(0.0, 0)

        stop_at = time.monotonic() + 8.0
        restart_at = stop_at + 4.0
        stopped = False
        restarted = False
        pre_stop_decoded = 0

        deadline = time.monotonic() + 26
        while time.monotonic() < deadline:
            line = receiver.stdout.readline()
            if not line:
                break
            line = line.strip()
            if not line.startswith("{"):
                print(line, flush=True)
                continue
            snap = json.loads(line)
            snapshots.append(snap)
            print(line, flush=True)

            now = time.monotonic()
            if not stopped and now >= stop_at:
                pre_stop_decoded = snap["decoded_frames"]
                print(f"marker=phase_2_sender_stop pre_stop_decoded={pre_stop_decoded}",
                      flush=True)
                reap(sender)
                sender = None
                stopped = True
            elif stopped and not restarted and now >= restart_at:
                print("marker=phase_3_sender_restart", flush=True)
                sender = spawn_sender(0.0, 0)
                restarted = True

        post = [s["decoded_frames"] for s in snapshots]
        final_decoded = post[-1] if post else 0
        failures = snapshots[-1]["decoder_failures"] if snapshots else -1
        recoveries = snapshots[-1].get("recoveries", -1)
        print(f"marker=result pre_stop_decoded={pre_stop_decoded} "
              f"final_decoded={final_decoded} decoder_failures={failures} "
              f"recoveries={recoveries}", flush=True)
        verdict = "PASS" if final_decoded > pre_stop_decoded and failures == 0 else "FAIL"
        print(f"marker=verdict restart_decode_recovery={verdict}", flush=True)
        return 0 if verdict == "PASS" else 1
    finally:
        reap(sender)
        reap(receiver)


if __name__ == "__main__":
    sys.exit(main())
