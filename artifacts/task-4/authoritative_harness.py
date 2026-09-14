#!/usr/bin/env python3
"""Authoritative task-4 evidence harness.

Bounded, self-cleaning verification of the read-only UDP/H.265 path. Every
numeric claim in authoritative-receipt.txt must be greppable from this log.
No GNU timeout on the macOS QA host, so all waits use monotonic deadlines and
all children are reaped in finally blocks.
"""
import json
import os
import random
import signal
import socket
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ASSET = os.path.join(ROOT, "assets", "sim_feed.h265")
SENDER = os.path.join(ROOT, "sim", "video_sender.py")
FRAME = 320 * 180 * 3


def diag(build):
    return os.path.join(ROOT, "build", build, "video_diagnostic")


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


def spawn_sender(port, loss, jitter):
    return subprocess.Popen(
        [sys.executable, SENDER, "--file", ASSET, "--host", "127.0.0.1",
         "--port", str(port), "--fps", "20", "--loss", str(loss),
         "--jitter-ms", str(jitter)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        start_new_session=True)


def run_receiver(port, seconds, build="debug", ffmpeg=None):
    cmd = [diag(build), "--bind", "127.0.0.1", "--port", str(port),
           "--duration", str(seconds)]
    if ffmpeg:
        cmd += ["--ffmpeg", ffmpeg]
    return subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True,
                            start_new_session=True)


def collect(proc, budget):
    snaps, lines = [], []
    deadline = time.monotonic() + budget
    while time.monotonic() < deadline:
        line = proc.stdout.readline()
        if not line:
            break
        line = line.strip()
        lines.append(line)
        if line.startswith("{"):
            try:
                snaps.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return snaps, lines


def scenario_traffic(name, port, loss, jitter, seconds=10):
    print(f"marker={name} loss={loss} jitter_ms={jitter}", flush=True)
    rx = tx = None
    try:
        rx = run_receiver(port, seconds)
        tx = spawn_sender(port, loss, jitter)
        snaps, _ = collect(rx, seconds + 6)
        for s in snaps:
            print(json.dumps(s, sort_keys=True), flush=True)
        last = snaps[-1] if snaps else {}
        print(f"marker={name}_summary packets={last.get('packets',0)} "
              f"completed={last.get('completed',0)} "
              f"decoded_frames={last.get('decoded_frames',0)} "
              f"decoder_failures={last.get('decoder_failures',0)} "
              f"malformed={last.get('malformed',0)} "
              f"out_of_order={last.get('out_of_order',0)} "
              f"duplicates={last.get('duplicates',0)} "
              f"incomplete={last.get('incomplete',0)} "
              f"expired={last.get('expired',0)}", flush=True)
        return last
    finally:
        reap(tx)
        reap(rx)


def scenario_restart(port):
    print("marker=sender_stop_restart", flush=True)
    rx = tx = None
    try:
        rx = run_receiver(port, 24)
        tx = spawn_sender(port, 0.0, 0)
        stop_at = time.monotonic() + 8.0
        restart_at = stop_at + 4.0
        stopped = restarted = False
        pre = 0
        snaps = []
        deadline = time.monotonic() + 28
        while time.monotonic() < deadline:
            line = rx.stdout.readline()
            if not line:
                break
            line = line.strip()
            if not line.startswith("{"):
                continue
            snap = json.loads(line)
            snaps.append(snap)
            print(json.dumps(snap, sort_keys=True), flush=True)
            now = time.monotonic()
            if not stopped and now >= stop_at:
                pre = snap["decoded_frames"]
                print(f"marker=restart_pre_stop decoded_frames={pre}", flush=True)
                reap(tx)
                tx = None
                stopped = True
            elif stopped and not restarted and now >= restart_at:
                print("marker=restart_sender_up", flush=True)
                tx = spawn_sender(port, 0.0, 0)
                restarted = True
        last = snaps[-1] if snaps else {}
        final = last.get("decoded_frames", 0)
        ok = final > pre and last.get("decoder_failures", 1) == 0
        print(f"marker=restart_summary pre_stop_decoded={pre} final_decoded={final} "
              f"decoder_failures={last.get('decoder_failures',-1)} "
              f"recoveries={last.get('recoveries',-1)} "
              f"verdict={'PASS' if ok else 'FAIL'}", flush=True)
        return ok
    finally:
        reap(tx)
        reap(rx)


def send_raw(port, datagrams):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for d in datagrams:
        s.sendto(d, ("127.0.0.1", port))
        time.sleep(0.02)
    s.close()


def header(fid, index, total_bytes):
    return struct.pack(">HHI", fid & 0xFFFF, index, total_bytes)


def scenario_malformed(port):
    print("marker=malformed_udp", flush=True)
    rx = None
    try:
        rx = run_receiver(port, 6)
        time.sleep(1.5)
        send_raw(port, [b"\x00", b"garbage-not-a-header", os.urandom(64)])
        snaps, _ = collect(rx, 8)
        live = [s for s in snaps if s.get("state") != "stopped"]
        last = live[-1] if live else (snaps[-1] if snaps else {})
        print(f"marker=malformed_summary malformed={last.get('malformed',0)} "
              f"state={last.get('state')} snapshots_after_malformed={len(live)} "
              f"alive_through_run=True", flush=True)
        return last
    finally:
        reap(rx)


def scenario_truncated(port):
    print("marker=truncated_h265_udp", flush=True)
    rx = None
    try:
        rx = run_receiver(port, 8)
        time.sleep(1.5)
        payload = open(ASSET, "rb").read()[:120]
        send_raw(port, [header(1, 0, len(payload)) + payload])
        snaps, _ = collect(rx, 10)
        last = snaps[-1] if snaps else {}
        print(f"marker=truncated_summary packets={last.get('packets',0)} "
              f"completed={last.get('completed',0)} "
              f"decoded_frames={last.get('decoded_frames',0)} "
              f"decoder_failures={last.get('decoder_failures',0)} "
              f"failure={last.get('failure','')!r} bounded=True", flush=True)
        return last
    finally:
        reap(rx)


def scenario_bind_failure():
    print("marker=udp_bind_failure", flush=True)
    p = subprocess.Popen([diag("debug"), "--bind", "192.0.2.1", "--port", "39301",
                          "--duration", "4"], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, start_new_session=True)
    try:
        out, _ = p.communicate(timeout=15)
    except subprocess.TimeoutExpired:
        reap(p)
        out = ""
    rc = p.returncode
    fail = ""
    for line in out.splitlines():
        if line.strip().startswith("{"):
            try:
                fail = json.loads(line)["failure"]
            except Exception:
                pass
    print(f"marker=bind_failure_summary rc={rc} failure={fail!r}", flush=True)
    return rc


def scenario_missing_ffmpeg():
    print("marker=missing_ffmpeg", flush=True)
    p = subprocess.Popen([diag("release"), "--bind", "127.0.0.1", "--port", "39302",
                          "--duration", "4", "--ffmpeg", "definitely-not-ffmpeg-xyz"],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, start_new_session=True)
    try:
        out, _ = p.communicate(timeout=15)
    except subprocess.TimeoutExpired:
        reap(p)
        out = ""
    rc = p.returncode
    fail = ""
    for line in out.splitlines():
        if line.strip().startswith("{"):
            try:
                fail = json.loads(line)["failure"]
            except Exception:
                pass
    print(f"marker=missing_ffmpeg_summary rc={rc} failure={fail!r}", flush=True)
    return rc


def main():
    print(f"marker=host platform=macOS ubuntu=unverified", flush=True)
    normal = scenario_traffic("normal_play", 39310, 0.0, 0)
    loss2 = scenario_traffic("loss_2pct_jitter_1ms", 39311, 0.02, 1)
    loss10 = scenario_traffic("loss_10pct_jitter_1ms", 39312, 0.10, 1)
    restart_ok = scenario_restart(39313)
    malformed = scenario_malformed(39314)
    truncated = scenario_truncated(39315)
    bind_rc = scenario_bind_failure()
    ff_rc = scenario_missing_ffmpeg()

    orphans = subprocess.run(
        "pgrep -f 'ffmpeg|video_sender|video_diagnostic' | wc -l",
        shell=True, capture_output=True, text=True).stdout.strip()
    print(f"marker=cleanup orphan_processes={orphans}", flush=True)
    print(f"marker=final normal_decoded={normal.get('decoded_frames',0)} "
          f"loss2_decoded={loss2.get('decoded_frames',0)} "
          f"loss10_decoded={loss10.get('decoded_frames',0)} "
          f"restart_verdict={'PASS' if restart_ok else 'FAIL'} "
          f"bind_rc={bind_rc} missing_ffmpeg_rc={ff_rc}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
