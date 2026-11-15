#!/usr/bin/env python3
"""
Soak test: the server under mixed load for a long time, to see whether
it slowly loses memory or descriptors.

Usage:
    python3 benchmarks/soak.py --build build-release
    python3 benchmarks/soak.py --build build-release --minutes 120
    python3 benchmarks/soak.py --build build --minutes 0.5 --cycle-seconds 5 --warmup-cycles 1

The time is cut into cycles. In each cycle the server gets, all at once:

  - echo of 64 bytes over 20 connections, as fast as they go,
  - 200 connections that are opened and then say nothing,
  - echo of 64 KiB over 5 connections,
  - light COMPUTE over 10 connections and PING over 5,
  - two clients that misbehave: resets, frames left unfinished, bad
    lengths, random bytes, pipelines and heavy work that are abandoned.

At the end of a cycle every client is gone, and the server is looked at
while it is idle. That is the only time two readings can be compared:
under load, what it holds depends on what is in flight.

It passes if, leaving out the first cycles as warm-up,

  - the number of open descriptors at the end of every cycle is what it
    was before the first,
  - the memory at the end of the cycles does not grow. Two ways of
    asking, and both must hold: the median of the last third is no more
    than 2 MiB or 5 % above the median of the first third, whichever is
    larger, and neither is a straight line fitted through all of them,
  - no well-behaved request failed, timed out or lost its connection,
  - the server is still running, and stops with exit code 0 when asked.

Memory here is what the process holds that cannot be had again from a
file: on Linux its resident memory plus what was swapped out, on macOS
its footprint. Resident memory alone will not do on macOS. Memory that
is leaked is never touched again, the system compresses it, and it
stops being counted as resident: a server that leaked 16 bytes per
request showed its resident memory fall after four cycles of rising.

Everything goes to benchmarks/results/<time>-soak-<system>-<poller>/.
"""

import argparse
import datetime
import json
import os
import platform
import random
import re
import signal
import socket
import statistics
import struct
import subprocess
import sys
import threading
import time

import analyze
import run
from run import protocol

import test_server as probe  # count_fds, settled_fd_count, reset

PORT = 18091
ADDRESS = ("127.0.0.1", PORT)
MIB = 1024 * 1024

# What the load generator is asked for in every cycle, one process each.
LOADS = (
    {"name": "echo", "type": "echo", "payload": 64, "connections": 20, "idle": 200},
    {"name": "large", "type": "echo", "payload": 65536, "connections": 5, "idle": 0},
    {"name": "compute", "type": "compute", "payload": 64, "iterations": 100,
     "connections": 10, "idle": 0},
    {"name": "ping", "type": "ping", "payload": 0, "connections": 5, "idle": 0},
)


def memory_bytes(pid):
    # See the note on memory at the top.
    try:
        with open(f"/proc/{pid}/status") as f:
            fields = dict(line.split(":", 1) for line in f if ":" in line)
        return (int(fields["VmRSS"].split()[0]) + int(fields["VmSwap"].split()[0])) * 1024
    except OSError:
        pass

    out = subprocess.run(
        ["footprint", "-f", "bytes", str(pid)], capture_output=True, text=True
    ).stdout
    found = re.search(r"Footprint: (\d+) B", out)
    if not found:
        raise RuntimeError(f"cannot read the memory of process {pid}")
    return int(found.group(1))


def fitted_growth(values):
    # The rise from the first to the last value of a straight line fitted
    # through them by least squares. One low or high reading moves it
    # little, where it would decide a comparison of two readings.
    n = len(values)
    if n < 2:
        return 0.0
    mean_x = (n - 1) / 2
    mean_y = sum(values) / n
    slope = sum((x - mean_x) * (y - mean_y) for x, y in enumerate(values)) / sum(
        (x - mean_x) ** 2 for x in range(n)
    )
    return slope * (n - 1)


def misbehave(seed, until):
    # One badly behaved client after another, until the time is up.
    rng = random.Random(seed)
    frame = protocol.encode(protocol.ECHO, 1, b"m" * 3000)
    heavy = protocol.encode(
        protocol.COMPUTE, 2, protocol.compute_payload(5000, bytes(1024))
    )

    while time.monotonic() < until:
        way = rng.randrange(7)
        try:
            s = socket.create_connection(ADDRESS, timeout=5)
            if way == 0:
                probe.reset(s)
                continue
            if way == 1:
                s.sendall(frame[: rng.randrange(1, len(frame))])
            elif way == 2:
                s.sendall(frame)
                probe.reset(s)
                continue
            elif way == 3:
                s.sendall(frame * 30)
            elif way == 4:
                s.sendall(heavy * 2)
            elif way == 5:
                s.sendall(struct.pack(">I", rng.choice([0, 5, 2**31, 2**32 - 1])))
            else:
                s.sendall(os.urandom(rng.randrange(1, 2000)))
            time.sleep(rng.random() * 0.05)
            s.close()
        except OSError:
            pass
        time.sleep(0.01)


def stats():
    with socket.create_connection(ADDRESS, timeout=10) as s:
        s.settimeout(10)
        return protocol.parse_stats(protocol.request(s, protocol.STATS, 1))


def wait_until_idle(pid, fds_before, timeout=60):
    # The server with nobody connected and nothing left to compute.
    # Returns (descriptors, counters).
    deadline = time.time() + timeout
    while True:
        fds = probe.count_fds(pid)
        counters = stats()
        idle = (
            fds == fds_before
            and counters["queue_depth"] == 0
            and counters["tasks_completed"] == counters["tasks_submitted"]
        )
        if idle or time.time() > deadline:
            return fds, counters
        time.sleep(0.2)


def one_cycle(args, directory, number, pid):
    until = time.monotonic() + args.cycle_seconds
    loadgens = []

    for load in LOADS:
        output = os.path.join(directory, f"loadgen-{load['name']}.tmp.json")
        loadgens.append((load, output, subprocess.Popen(
            [
                os.path.join(args.build, "loadgen"),
                "--port", str(PORT),
                "--connections", str(load["connections"]),
                "--idle-connections", str(load["idle"]),
                "--threads", "1",
                "--warmup", "0",
                "--duration", str(args.cycle_seconds),
                "--type", load["type"],
                "--payload", str(load["payload"]),
                "--iterations", str(load.get("iterations", 0)),
                "--output", output,
            ],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )))

    bad = [
        threading.Thread(target=misbehave, args=(number * 10 + i, until), daemon=True)
        for i in range(2)
    ]
    for thread in bad:
        thread.start()

    # What it holds under load, for the record. Not part of the verdict.
    peak_memory = 0
    peak_fds = 0
    while time.monotonic() < until:
        time.sleep(min(2.0, max(0.0, until - time.monotonic())))
        try:
            peak_memory = max(peak_memory, memory_bytes(pid))
            peak_fds = max(peak_fds, probe.count_fds(pid))
        except (RuntimeError, KeyError):
            break  # the server is gone, which the caller finds out

    requests = {}
    for load, output, process in loadgens:
        process.wait()
        with open(output) as f:
            result = json.load(f)
        os.remove(output)
        requests[load["name"]] = {
            **{kind: result["requests"][kind]
               for kind in ("completed", "busy", "errors", "timeouts", "connection_errors")},
            "connect_failures": result["connections"]["connect_failures"],
            "idle_lost": result["connections"]["idle_lost"],
        }

    for thread in bad:
        thread.join()

    return requests, peak_memory, peak_fds


def verdict(samples, warmup, fds_before, exit_code):
    # Returns [(what was checked, passed, detail)].
    judged = samples[warmup:]
    checks = []

    wrong_fds = [s["cycle"] for s in judged if s["fds"] != fds_before]
    checks.append((
        "descriptors at the end of every cycle are what they were at the start",
        not wrong_fds,
        f"{fds_before} throughout" if not wrong_fds else f"differs after cycles {wrong_fds}",
    ))

    memory = [s["memory_bytes"] for s in judged]
    third = max(1, len(judged) // 3)
    early = statistics.median(memory[:third])
    late = statistics.median(memory[-third:])
    fitted = fitted_growth(memory)
    allowed = max(2 * MIB, early * 0.05)
    checks.append((
        "memory at the end of the cycles does not grow",
        late - early <= allowed and fitted <= allowed,
        f"first third {early / MIB:.2f} MiB, last third {late / MIB:.2f} MiB, "
        f"fitted growth over the run {fitted / MIB:+.2f} MiB, "
        f"allowed {allowed / MIB:.1f} MiB",
    ))

    failed = {}
    for s in samples:
        for name, counts in s["requests"].items():
            for kind in ("busy", "errors", "timeouts", "connection_errors",
                         "connect_failures", "idle_lost"):
                if counts[kind]:
                    failed[f"{name} {kind}"] = failed.get(f"{name} {kind}", 0) + counts[kind]
    checks.append((
        "no well-behaved request failed",
        not failed,
        "none" if not failed else ", ".join(f"{k}: {v}" for k, v in failed.items()),
    ))

    thrown = samples[-1]["stats"]["tasks_failed"] if samples else 0
    checks.append(("no request failed inside a worker", thrown == 0, f"{thrown}"))

    checks.append((
        "the server stops with exit code 0 when asked",
        exit_code == 0,
        f"exit code {exit_code}",
    ))

    return checks


def write_report(directory, record):
    env = record["environment"]
    build = env["build"]
    samples = record["samples"]
    lines = []
    add = lines.append

    add("# Soak test")
    add("")
    add(f"**{'Passed' if record['passed'] else 'FAILED'}.** "
        f"{record['minutes']:g} minutes in {len(samples)} cycles of "
        f"{record['cycle_seconds']:g} s, the first {record['warmup_cycles']} left out as warm-up.")
    add("")
    add("| Check | Result | |")
    add("| --- | --- | --- |")
    for what, passed, detail in record["checks"]:
        add(f"| {what} | {'pass' if passed else '**fail**'} | {detail} |")
    add("")
    add("## Where this was run")
    add("")
    add("| | |")
    add("| --- | --- |")
    add(f"| Date (UTC) | {env['date_utc']} |")
    add(f"| Machine | {env['cpu']}, {env['cores']} cores, {env['machine']} |")
    add(f"| OS | {env['os']} ({env['kernel']}) |")
    if env.get("power"):
        add(f"| Power | {env['power']} |")
    if env["in_container"]:
        add("| Container | Yes, Linux in a virtual machine. |")
    add(f"| Compiler | {build['compiler']} |")
    add(f"| Build | {build['build_type']}, flags `{build['flags'].strip()}`, sanitizer {build['sanitizer']} |")
    add(f"| Commit | {env['git_commit'] or 'unknown'}"
        f"{' plus uncommitted changes' if env['git_uncommitted_changes'] else ''} |")
    add(f"| Server | `{' '.join(record['server_command'])}` |")
    add(f"| Command | `{record['command']}` |")
    add("")
    add("## What the server did")
    add("")
    total = {}
    for s in samples:
        for name, counts in s["requests"].items():
            total[name] = total.get(name, 0) + counts["completed"]
    add("| Load | Requests answered |")
    add("| --- | ---: |")
    for name, count in total.items():
        add(f"| {name} | {count:,} |")
    add(f"| **all** | **{sum(total.values()):,}** |")
    add("")
    last = samples[-1]["stats"]
    add(f"By the server's own count it handed {last['tasks_submitted']:,} requests to "
        f"its workers, refused {last['tasks_rejected']:,} for a full queue, and "
        f"{last['tasks_failed']:,} failed in a worker. The misbehaving clients are in "
        "those numbers and not in the table.")
    add("")
    add(f"The server wrote {record['server_log_lines']:,} lines to its log, "
        "which is `server.log` here. It reports timeouts and connection errors "
        "at most once a second each.")
    add("")
    add("## The server at the end of each cycle")
    add("")
    add("Idle, with every client gone. Peak is the most seen during the cycle.")
    add("Memory is resident memory plus what was swapped out on Linux, and the")
    add("footprint on macOS, which also counts memory the system has compressed.")
    add("")
    add("![memory](memory.svg)")
    add("")
    add("| Cycle | Minute | Memory | Descriptors | Peak memory | Peak descriptors |")
    add("| ---: | ---: | ---: | ---: | ---: | ---: |")
    for s in samples:
        note = " (warm-up)" if s["cycle"] <= record["warmup_cycles"] else ""
        add(f"| {s['cycle']}{note} | {s['minute']:.1f} | {s['memory_bytes'] / MIB:.2f} MiB | "
            f"{s['fds']} | {s['peak_memory_bytes'] / MIB:.1f} MiB | {s['peak_fds']} |")
    add("")
    add(f"Before the first cycle: {record['memory_before'] / MIB:.2f} MiB, "
        f"{record['fds_before']} descriptors.")
    add("")

    with open(os.path.join(directory, "report.md"), "w") as f:
        f.write("\n".join(lines))

    points = [(s["minute"], s["memory_bytes"] / MIB) for s in samples]
    ticks = sorted({round(s["minute"]) for s in samples[:: max(1, len(samples) // 8)]})
    analyze.line_chart(
        os.path.join(directory, "memory.svg"),
        "Memory of the idle server at the end of each cycle",
        "minutes",
        "memory, MiB",
        [("at the end of a cycle", [(x, y, y, y) for x, y in points])],
        [(t, str(t)) for t in ticks] or [(0, "0")],
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("--build", required=True)
    parser.add_argument("--minutes", type=float, default=30.0)
    parser.add_argument("--cycle-seconds", type=float, default=60.0)
    parser.add_argument("--warmup-cycles", type=int, default=3)
    parser.add_argument("--poller", choices=("poll", "epoll"))
    parser.add_argument("--results", default=os.path.join(run.HERE, "results"))
    args = parser.parse_args()

    cycles = max(args.warmup_cycles + 3, round(args.minutes * 60 / args.cycle_seconds))
    run.raise_open_files_limit(4096)

    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    system = "macos" if sys.platform == "darwin" else platform.system().lower()
    poller = args.poller or ("epoll" if sys.platform.startswith("linux") else "poll")
    directory = os.path.join(args.results, f"{stamp}-soak-{system}-{poller}")
    os.makedirs(directory)

    server_command = [
        os.path.join(args.build, "server"),
        "--port", str(PORT),
        "--workers", "4",
        "--poller", poller,
    ]
    log_path = os.path.join(directory, "server.log")

    with open(log_path, "w") as log:
        server = subprocess.Popen(server_command, stdout=subprocess.DEVNULL, stderr=log)
        samples = []
        exit_code = None

        try:
            deadline = time.time() + 10
            while True:
                try:
                    socket.create_connection(ADDRESS, timeout=1).close()
                    break
                except OSError:
                    if server.poll() is not None or time.time() > deadline:
                        raise SystemExit("the server did not start")
                    time.sleep(0.02)

            fds_before = probe.settled_fd_count(server.pid)
            memory_before = memory_bytes(server.pid)
            started = time.monotonic()

            for number in range(1, cycles + 1):
                requests, peak_memory, peak_fds = one_cycle(args, directory, number, server.pid)

                if server.poll() is not None:
                    print(f"the server ended in cycle {number}, exit code {server.returncode}")
                    break

                fds, counters = wait_until_idle(server.pid, fds_before)
                sample = {
                    "cycle": number,
                    "minute": round((time.monotonic() - started) / 60, 2),
                    "memory_bytes": memory_bytes(server.pid),
                    "fds": fds,
                    "peak_memory_bytes": peak_memory,
                    "peak_fds": peak_fds,
                    "stats": counters,
                    "requests": requests,
                }
                samples.append(sample)

                answered = sum(r["completed"] for r in requests.values())
                print(f"cycle {number:3}/{cycles}  minute {sample['minute']:6.1f}  "
                      f"memory {sample['memory_bytes'] / MIB:7.2f} MiB  descriptors {fds}  "
                      f"answered {answered:,}", flush=True)

            if server.poll() is None:
                server.send_signal(signal.SIGTERM)
                exit_code = server.wait(timeout=30)
            else:
                exit_code = server.returncode
        finally:
            if server.poll() is None:
                server.kill()
                server.wait()

    with open(log_path) as f:
        log_lines = sum(1 for _ in f)

    checks = verdict(samples, args.warmup_cycles, fds_before, exit_code)
    completed_all = len(samples) == cycles
    passed = completed_all and all(ok for _, ok, _ in checks)

    record = {
        "passed": passed,
        "checks": checks,
        "minutes": round(samples[-1]["minute"], 1) if samples else 0,
        "cycle_seconds": args.cycle_seconds,
        "cycles_planned": cycles,
        "warmup_cycles": args.warmup_cycles,
        "environment": run.environment(args.build),
        "command": " ".join(["benchmarks/soak.py"] + sys.argv[1:]),
        "server_command": server_command,
        "loads": LOADS,
        "fds_before": fds_before,
        "memory_before": memory_before,
        "server_exit_code": exit_code,
        "server_log_lines": log_lines,
        "samples": samples,
    }

    with open(os.path.join(directory, "soak.json"), "w") as f:
        json.dump(record, f, indent=1)
        f.write("\n")

    if samples:
        write_report(directory, record)

    print()
    for what, ok, detail in checks:
        print(f"{'pass' if ok else 'FAIL'}  {what}: {detail}")
    if not completed_all:
        print(f"FAIL  only {len(samples)} of {cycles} cycles were completed")
    print(f"\n{'PASSED' if passed else 'FAILED'}, see {directory}/report.md")

    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
