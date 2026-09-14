#!/usr/bin/env python3
"""Bounded lifecycle harness for plan task 5.

Runs consecutive start/exit/restart cycles against the native terminal and
asserts structured-log integrity. Every scenario has a monotonic deadline and
every spawned process is killed in a finally block, because this macOS QA host
has no GNU timeout.
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
BINARY = os.path.join(ROOT, "build", "debug", "rm_terminal")
RECORD = re.compile(
    r"^ts=(?P<ts>\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z) "
    r"level=(?P<level>debug|info|warning|error) "
    r"event=(?P<event>[a-z_]+)(?P<fields>(?: [a-z_]+=[^ ]*)*)$"
)

failures = []


def check(condition, label):
    status = "ok" if condition else "FAIL"
    print(f"  [{status}] {label}")
    if not condition:
        failures.append(label)


def run(args, cwd, deadline_s, env=None):
    merged = dict(os.environ)
    if env:
        merged.update(env)
    proc = subprocess.Popen(
        args,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        start_new_session=True,
        env=merged,
    )
    try:
        out, err = proc.communicate(timeout=deadline_s)
        return proc.returncode, out.decode(errors="replace"), err.decode(errors="replace")
    except subprocess.TimeoutExpired:
        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        proc.wait(timeout=5)
        return None, "", "timeout"
    finally:
        if proc.poll() is None:
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            except ProcessLookupError:
                pass
            proc.wait(timeout=5)


def parse(path):
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as handle:
        return [RECORD.match(line.strip()) for line in handle if line.strip()]


def events(path):
    return [m.group("event") for m in parse(path) if m]


def scenario_repeated_cycles(work):
    print("marker=repeated_cycles")
    log = os.path.join(work, "cycles.log")
    conf = os.path.join(work, "cycles.conf")
    with open(conf, "w", encoding="utf-8") as handle:
        handle.write(f"log_destination={log}\nlog_level=info\nstale_window_ms=400\n")

    cycles = 5
    for index in range(cycles):
        rc, out, err = run([BINARY, "--config", conf, "--safe-smoke"], work, 20)
        check(rc == 0, f"cycle {index + 1} exit 0")
        check("READ-ONLY SIMULATION SAFE" in err, f"cycle {index + 1} banner present")

    records = parse(log)
    check(all(m is not None for m in records), "every line matches the record grammar")
    names = [m.group("event") for m in records if m]
    check(names.count("startup") == cycles, f"{cycles} startup records")
    check(names.count("shutdown") == cycles, f"{cycles} shutdown records")
    check(names.count("readonly_block") == cycles, f"{cycles} readonly_block records")
    check(names[0] == "startup", "first record is startup")
    check(names[-1] == "shutdown", "last record is shutdown")
    check(len(names) == cycles * 3, "restart appends rather than truncating")

    stamps = [m.group("ts") for m in records if m]
    check(stamps == sorted(stamps), "timestamps are monotonically ordered")
    print(f"marker=cycles cycles={cycles} records={len(names)}")


def scenario_config_precedence(work):
    print("marker=config_precedence")
    flag_log = os.path.join(work, "flag.log")
    env_log = os.path.join(work, "env.log")
    cwd_log = os.path.join(work, "cwd.log")
    for name, dest, port in (
        ("flag.conf", flag_log, 4444),
        ("env.conf", env_log, 5555),
        ("rm_terminal.conf", cwd_log, 6666),
    ):
        with open(os.path.join(work, name), "w", encoding="utf-8") as handle:
            handle.write(f"log_destination={dest}\nmqtt_port={port}\n")

    rc, _, _ = run(
        [BINARY, "--config", "flag.conf", "--safe-smoke"],
        work,
        20,
        env={"RM_TERMINAL_CONFIG": "env.conf"},
    )
    check(rc == 0, "flag beats env exit 0")
    check(any("mqtt_port=4444" in m.group("fields") for m in parse(flag_log) if m),
          "--config wins over RM_TERMINAL_CONFIG")

    rc, _, _ = run([BINARY, "--safe-smoke"], work, 20, env={"RM_TERMINAL_CONFIG": "env.conf"})
    check(rc == 0, "env exit 0")
    check(any("mqtt_port=5555" in m.group("fields") for m in parse(env_log) if m),
          "RM_TERMINAL_CONFIG wins over ./rm_terminal.conf")

    rc, _, _ = run([BINARY, "--safe-smoke"], work, 20)
    check(rc == 0, "cwd config exit 0")
    check(any("mqtt_port=6666" in m.group("fields") for m in parse(cwd_log) if m),
          "./rm_terminal.conf used when nothing else is given")

    bare = os.path.join(work, "bare")
    os.makedirs(bare, exist_ok=True)
    rc, _, _ = run([BINARY, "--safe-smoke"], bare, 20)
    check(rc == 0, "missing config exit 0")
    check(any("mqtt_port=3333" in m.group("fields")
              for m in parse(os.path.join(bare, "rm_terminal.log")) if m),
          "built-in defaults apply with no config file")
    print("marker=precedence tiers=4")


def scenario_malformed_config(work):
    print("marker=malformed_config")
    cases = [
        ("mqtt_port=abc\n", "mqtt_port"),
        ("mqtt_port=0\n", "mqtt_port"),
        ("stale_window_ms=-5\n", "stale_window_ms"),
        ("log_level=verbose\n", "log_level"),
        ("mqtt_host=\n", "mqtt_host"),
        ("bogus=1\n", "bogus"),
        ("mqtt_port\n", "key=value"),
    ]
    for body, needle in cases:
        path = os.path.join(work, "bad.conf")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(body)
        rc, out, err = run([BINARY, "--config", path], work, 20)
        label = body.strip()
        check(rc == 4, f"{label} exits 4")
        check(needle in err, f"{label} names the offending key")
        check(out == "", f"{label} prints nothing on stdout")
    print(f"marker=malformed cases={len(cases)}")


def scenario_exit_codes(work):
    print("marker=exit_codes")
    rc, _, err = run([BINARY, "--safe-smoke"], work, 20)
    check(rc == 0, "safe-smoke exits 0")
    rc, _, err = run([BINARY, "--unknown"], work, 20)
    check(rc == 2, "unknown argument exits 2")
    check(0 < len(err) <= 256, "bad-argument stderr is bounded under 256 bytes")
    check("SIMULATION SAFE" not in err, "bad argument does not print the safe banner")
    rc, _, _ = run([BINARY, "--diagnostic", "127.0.0.1", "0", "1"], work, 20)
    check(rc == 2, "invalid diagnostic port exits 2")
    print("marker=exit_codes verified=4")


def scenario_log_level_filter(work):
    print("marker=log_level_filter")
    log = os.path.join(work, "quiet.log")
    conf = os.path.join(work, "quiet.conf")
    with open(conf, "w", encoding="utf-8") as handle:
        handle.write(f"log_destination={log}\nlog_level=error\n")
    rc, _, _ = run([BINARY, "--config", conf, "--safe-smoke"], work, 20)
    check(rc == 0, "quiet run exits 0")
    names = events(log)
    check("startup" not in names, "info startup filtered at error level")
    check("readonly_block" not in names, "warning readonly_block filtered at error level")
    print(f"marker=filter records={len(names)}")


def scenario_unwritable_destination(work):
    print("marker=unwritable_destination")
    conf = os.path.join(work, "unwritable.conf")
    with open(conf, "w", encoding="utf-8") as handle:
        handle.write("log_destination=/nonexistent-directory-xyz/rm.log\n")
    rc, out, err = run([BINARY, "--config", conf], work, 20)
    check(rc == 4, "unwritable log destination exits 4")
    check("log error" in err, "unwritable destination reports a log error")
    print("marker=unwritable verified=1")


def main():
    started = time.monotonic()
    if not os.path.exists(BINARY):
        print(f"marker=abort reason=missing_binary path={BINARY}")
        return 1

    work = tempfile.mkdtemp(prefix="rm-task5-")
    try:
        scenario_repeated_cycles(work)
        scenario_config_precedence(work)
        scenario_malformed_config(work)
        scenario_exit_codes(work)
        scenario_log_level_filter(work)
        scenario_unwritable_destination(work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
        leaked = subprocess.run(
            ["pgrep", "-f", "rm_terminal"], capture_output=True, text=True
        ).stdout.strip()
        orphans = [pid for pid in leaked.splitlines() if pid]
        print(f"marker=cleanup orphan_processes={len(orphans)}")
        if orphans:
            failures.append("orphan processes survived")

    elapsed = time.monotonic() - started
    print(f"marker=summary failures={len(failures)} elapsed_s={elapsed:.1f}")
    if failures:
        for item in failures:
            print(f"  failed: {item}")
        return 1
    print("marker=verdict PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
