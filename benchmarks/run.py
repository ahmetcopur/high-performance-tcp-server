#!/usr/bin/env python3
"""
Runs benchmark suites against the server and keeps everything needed to
check a number later: the raw result of every repetition, what the
server was doing while it ran, and the machine, compiler and flags.

    python3 benchmarks/run.py --build build-release --suite connections

Each repetition starts a fresh server, lets the load generator connect,
warm up and measure, and stops the server again. Results go to
benchmarks/results/<timestamp>-<suite>/. Turn them into tables and plots
with benchmarks/analyze.py.

Suites:
    quick        a few seconds, to see that everything works
    connections  echo of 64 bytes over 1 to 10,000 connections
    payloads     echo of 64 bytes to 64 KiB over 100 connections
    compute      three costs of COMPUTE over 100 connections
    mix          100 busy connections beside 0 to 5,000 idle ones
    workers      1, 2, 4 and 8 workers with echo, light and heavy compute
    latency      open loop at fractions of the measured capacity
    full         all of the above except quick
"""

import argparse
import datetime
import json
import os
import platform
import resource
import signal
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

sys.path.insert(0, os.path.join(ROOT, "tests"))
import protocol  # noqa: E402

PORT = 18090

LIGHT = {"type": "compute", "iterations": 100, "payload": 64}
MEDIUM = {"type": "compute", "iterations": 1000, "payload": 1024}
HEAVY = {"type": "compute", "iterations": 10000, "payload": 1024}


def echo(payload):
    return {"type": "echo", "payload": payload}


def cell(name, load, connections=100, workers=4, idle=0, rate_fraction=None):
    return {
        "name": name,
        # Filled in by suite_cells(): which suite the cell belongs to.
        "group": None,
        "workers": workers,
        "connections": connections,
        "idle_connections": idle,
        "load": load,
        "rate_fraction": rate_fraction,
    }


def suite_cells(suite):
    cells = cells_of(suite)

    for c in cells:
        if c["group"] is None:
            c["group"] = suite

    return cells


def cells_of(suite):
    if suite == "quick":
        return [
            cell("echo-64B-10c", echo(64), connections=10),
            cell("compute-light-10c", LIGHT, connections=10),
        ]

    if suite == "connections":
        return [
            cell(f"echo-64B-{n}c", echo(64), connections=n)
            for n in (1, 10, 100, 1000, 5000, 10000)
        ]

    if suite == "payloads":
        return [
            cell(f"echo-{size}B-100c", echo(size))
            for size in (64, 256, 1024, 4096, 65536)
        ]

    if suite == "compute":
        return [
            cell("compute-light-100c", LIGHT),
            cell("compute-medium-100c", MEDIUM),
            cell("compute-heavy-100c", HEAVY),
        ]

    if suite == "mix":
        return [
            cell(f"echo-64B-100c-{idle}idle", echo(64), idle=idle)
            for idle in (0, 1000, 5000)
        ]

    if suite == "workers":
        return [
            cell(f"{name}-{workers}w", load, workers=workers)
            for name, load in (
                ("echo-64B", echo(64)),
                ("compute-light", LIGHT),
                ("compute-heavy", HEAVY),
            )
            for workers in (1, 2, 4, 8)
        ]

    if suite == "latency":
        # The first cell measures what the server can do. The others ask
        # for a fraction of that at a fixed rate.
        return [cell("echo-64B-100c-capacity", echo(64))] + [
            cell(
                f"echo-64B-100c-{int(fraction * 100)}pct",
                echo(64),
                rate_fraction=fraction,
            )
            for fraction in (0.25, 0.5, 0.75, 0.9)
        ]

    if suite == "full":
        cells = []
        for name in ("connections", "payloads", "compute", "mix", "workers", "latency"):
            cells += suite_cells(name)
        return cells

    raise SystemExit(f"unknown suite: {suite}")


def output_of(command):
    try:
        return subprocess.check_output(
            command, text=True, stderr=subprocess.DEVNULL
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def cpu_model():
    if sys.platform == "darwin":
        return output_of(["sysctl", "-n", "machdep.cpu.brand_string"])
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.lower().startswith(("model name", "hardware")):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or None


def os_description():
    if sys.platform == "darwin":
        return f"macOS {platform.mac_ver()[0]}"
    try:
        with open("/etc/os-release") as f:
            for line in f:
                if line.startswith("PRETTY_NAME="):
                    return line.split("=", 1)[1].strip().strip('"')
    except OSError:
        pass
    return platform.system()


def power_source():
    # A laptop on battery may run slower than on mains.
    if sys.platform != "darwin":
        return None
    out = output_of(["pmset", "-g", "batt"])
    if not out:
        return None
    return out.splitlines()[0].replace("Now drawing from ", "").strip("'")


def environment(build_dir):
    with open(os.path.join(build_dir, "build_info.json")) as f:
        build = json.load(f)

    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    status = output_of(["git", "-C", ROOT, "status", "--porcelain"])

    return {
        "date_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(
            timespec="seconds"
        ),
        "os": os_description(),
        "kernel": f"{platform.system()} {platform.release()}",
        "machine": platform.machine(),
        "cpu": cpu_model(),
        "cores": os.cpu_count(),
        "power": power_source(),
        # Docker on a Mac runs Linux in a virtual machine. Numbers from
        # there are not numbers from a Linux machine.
        "in_container": os.path.exists("/.dockerenv"),
        "python": platform.python_version(),
        "open_files_limit": {"soft": soft, "hard": hard},
        "build": build,
        "git_commit": output_of(["git", "-C", ROOT, "rev-parse", "HEAD"]),
        "git_uncommitted_changes": bool(status) if status is not None else None,
    }


def raise_open_files_limit(wanted):
    # Returns how many descriptors a process may have after raising the
    # soft limit as far as it will go.
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)

    if soft >= wanted:
        return soft

    target = wanted if hard == resource.RLIM_INFINITY else min(wanted, hard)

    # macOS allows no more than 10,240 per process whatever the limit
    # says, so that is tried second.
    for limit in (target, 10240):
        try:
            resource.setrlimit(resource.RLIMIT_NOFILE, (limit, hard))
            break
        except (ValueError, OSError):
            continue

    return resource.getrlimit(resource.RLIMIT_NOFILE)[0]


def cpu_seconds(pid):
    with open(f"/proc/{pid}/stat") as f:
        fields = f.read().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def ps_sample(pid):
    # CPU time and resident memory where there is no /proc. One ps call
    # for both, because starting a process is not free and this runs
    # while the server is being measured.
    out = output_of(["ps", "-o", "time=,rss=", "-p", str(pid)])
    if not out:
        return None, None
    cpu_text, rss_kb = out.split()
    seconds = 0.0
    for part in cpu_text.split(":"):
        seconds = seconds * 60 + float(part)
    return seconds, int(rss_kb) * 1024


def linux_status(pid):
    # Resident memory, and context switches summed over every thread.
    # None where /proc does not exist.
    try:
        tasks = os.listdir(f"/proc/{pid}/task")
    except OSError:
        return None

    switches = {"voluntary": 0, "involuntary": 0}
    rss_kb = None

    for task in tasks:
        try:
            with open(f"/proc/{pid}/task/{task}/status") as f:
                for line in f:
                    if line.startswith("voluntary_ctxt_switches"):
                        switches["voluntary"] += int(line.split()[1])
                    elif line.startswith("nonvoluntary_ctxt_switches"):
                        switches["involuntary"] += int(line.split()[1])
                    elif line.startswith("VmRSS") and rss_kb is None:
                        rss_kb = int(line.split()[1])
        except OSError:
            continue

    return {
        "rss_bytes": rss_kb * 1024 if rss_kb is not None else None,
        "context_switches": switches,
        "threads": len(tasks),
        "open_files": len(os.listdir(f"/proc/{pid}/fd")),
    }


class Sampler(threading.Thread):
    """Looks at the server four times a second while the load runs."""

    def __init__(self, pid):
        super().__init__(daemon=True)
        self.pid = pid
        self.samples = []
        self.stopping = threading.Event()

    def stats(self, connection):
        try:
            frame = protocol.request(connection, protocol.STATS, 1)
            # None means the server closed the connection.
            return protocol.parse_stats(frame) if frame else None
        except (OSError, ValueError, EOFError):
            return None

    def run(self):
        try:
            connection = socket.create_connection(("127.0.0.1", PORT), timeout=2)
            connection.settimeout(2)
        except OSError:
            connection = None

        while not self.stopping.is_set():
            sample = {
                "t": time.time(),
                "stats": self.stats(connection) if connection else None,
            }

            status = linux_status(self.pid)
            if status is not None:
                sample["cpu_s"] = cpu_seconds(self.pid)
                sample.update(status)
            else:
                sample["cpu_s"], sample["rss_bytes"] = ps_sample(self.pid)

            self.samples.append(sample)
            self.stopping.wait(0.25)

        if connection:
            connection.close()

    def stop(self):
        self.stopping.set()
        self.join()
        return self.samples


def summarize(samples, measured_from, measured_to):
    # What the server did during the measured part of the run. The
    # sampler's own connection is one of the connections it reports.
    inside = [s for s in samples if measured_from <= s["t"] <= measured_to]
    if len(inside) < 2:
        return None

    first, last = inside[0], inside[-1]
    elapsed = last["t"] - first["t"]
    summary = {"samples": len(inside), "seconds": round(elapsed, 3)}

    if first["cpu_s"] is not None and last["cpu_s"] is not None:
        # 1.0 is one core fully used.
        summary["cpu_cores"] = round((last["cpu_s"] - first["cpu_s"]) / elapsed, 3)

    rss = [s["rss_bytes"] for s in inside if s.get("rss_bytes")]
    if rss:
        summary["rss_bytes_max"] = max(rss)

    if "context_switches" in first:
        for kind in ("voluntary", "involuntary"):
            delta = last["context_switches"][kind] - first["context_switches"][kind]
            summary[f"{kind}_switches_per_s"] = round(delta / elapsed, 1)
        summary["open_files_max"] = max(s["open_files"] for s in inside)

    stats = [s["stats"] for s in inside if s["stats"]]
    if len(stats) >= 2:
        a, b = stats[0], stats[-1]
        uptime = (b["uptime_us"] - a["uptime_us"]) or 1
        summary["queue_depth_max"] = max(s["queue_depth"] for s in stats)
        summary["connections_max"] = max(s["connections"] for s in stats)
        summary["tasks_rejected"] = b["tasks_rejected"] - a["tasks_rejected"]
        # Share of the time the workers spent on requests.
        summary["worker_utilization"] = round(
            (b["worker_busy_us"] - a["worker_busy_us"]) / (uptime * b["workers"]), 3
        )

    return summary


def wait_until_listening(proc):
    deadline = time.time() + 10
    while True:
        if proc.poll() is not None:
            raise SystemExit("the server exited at once, see its message above")
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=1).close()
            return
        except OSError:
            if time.time() > deadline:
                raise SystemExit("the server did not start listening")
            time.sleep(0.02)


def pinned(command, cpus):
    return ["taskset", "-c", cpus, *command] if cpus else command


def run_once(args, cell_config, rate):
    """One repetition: fresh server, one load generator run."""
    connections = cell_config["connections"]
    idle = cell_config["idle_connections"]
    load = cell_config["load"]
    threads = min(args.client_threads, connections)

    server_command = pinned(
        [
            os.path.join(args.build, "server"),
            "--port", str(PORT),
            "--workers", str(cell_config["workers"]),
            "--max-connections", str(connections + idle + 100),
            # Idle connections must outlast the run.
            "--idle-timeout-ms", "600000",
            *(["--poller", args.poller] if args.poller else []),
        ],
        args.server_cpus,
    )

    result_path = os.path.join(args.directory, "loadgen.tmp.json")
    loadgen_command = pinned(
        [
            os.path.join(args.build, "loadgen"),
            "--port", str(PORT),
            "--connections", str(connections),
            "--idle-connections", str(idle),
            "--threads", str(threads),
            "--warmup", str(args.warmup),
            "--duration", str(args.duration),
            "--type", load["type"],
            "--payload", str(load["payload"]),
            "--iterations", str(load.get("iterations", 0)),
            "--rate", str(rate),
            "--output", result_path,
            *(["--poller", args.poller] if args.poller else []),
        ],
        args.client_cpus,
    )

    server = subprocess.Popen(
        server_command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True
    )

    try:
        wait_until_listening(server)
        sampler = Sampler(server.pid)
        sampler.start()

        started = time.time()
        started_awake = time.monotonic()
        loadgen = subprocess.run(loadgen_command, capture_output=True, text=True)
        finished = time.time()
        awake = time.monotonic() - started_awake

        samples = sampler.stop()
    finally:
        server.send_signal(signal.SIGINT)
        try:
            _, server_stderr = server.communicate(timeout=30)
        except subprocess.TimeoutExpired:
            server.kill()
            _, server_stderr = server.communicate()

    if loadgen.returncode != 0:
        raise SystemExit(f"the load generator failed:\n{loadgen.stderr}")

    with open(result_path) as f:
        result = json.load(f)
    os.remove(result_path)

    # The measured part is the last `duration` seconds of the load
    # generator's life. Leave a margin at both ends.
    margin = min(0.3, args.duration * 0.1)
    measured_to = finished - margin
    measured_from = finished - args.duration + margin

    # The monotonic clock stops while the machine sleeps and the wall
    # clock does not. If they disagree, the machine slept during the run
    # and the numbers describe a sleeping machine. A run that took much
    # longer than it should have is not to be trusted either.
    expected = args.warmup + args.duration
    slept = (finished - started) - awake > 1.0
    stalled = slept or (finished - started) > expected * 1.5 + 5

    return {
        "cell": cell_config["name"],
        "server_command": server_command,
        "loadgen_command": loadgen_command,
        "wall_seconds": round(finished - started, 3),
        "machine_slept": slept,
        "suspect": stalled,
        "loadgen": result,
        "server": summarize(samples, measured_from, measured_to),
        "server_samples": samples,
        "server_stderr": server_stderr.strip().splitlines()[-5:],
    }


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--build", required=True, help="build directory")
    parser.add_argument("--suite", default="quick")
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--warmup", type=float, default=2.0)
    parser.add_argument("--duration", type=float, default=5.0)
    parser.add_argument("--poller", choices=("poll", "epoll"))
    parser.add_argument("--client-threads", type=int, default=4)
    parser.add_argument("--server-cpus", help="Linux: pin the server, e.g. 0-3")
    parser.add_argument("--client-cpus", help="Linux: pin the client, e.g. 4-7")
    parser.add_argument("--results", default=os.path.join(HERE, "results"))
    parser.add_argument(
        "--any-build",
        action="store_true",
        help="allow a build that is not Release",
    )
    args = parser.parse_args()

    if args.suite == "quick":
        args.repetitions, args.warmup, args.duration = 1, 0.5, 1.0

    env = environment(args.build)

    if env["build"]["build_type"] != "Release" and not args.any_build:
        raise SystemExit(
            f"{args.build} is a {env['build']['build_type']} build. Benchmark a "
            "Release build, or pass --any-build if you know what you are doing."
        )

    if (args.server_cpus or args.client_cpus) and not sys.platform.startswith("linux"):
        raise SystemExit("pinning to CPUs needs taskset, which is Linux only")

    cells = suite_cells(args.suite)

    # Client and server each need a descriptor per connection.
    largest = max(c["connections"] + c["idle_connections"] for c in cells)
    limit = raise_open_files_limit(largest + 200)
    env["open_files_limit"]["raised_to"] = limit

    skipped = [c["name"] for c in cells if c["connections"] + c["idle_connections"] + 100 > limit]
    cells = [c for c in cells if c["name"] not in skipped]

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    args.directory = os.path.join(args.results, f"{stamp}-{args.suite}")
    os.makedirs(os.path.join(args.directory, "runs"))

    index = {
        "suite": args.suite,
        "environment": env,
        "settings": {
            "repetitions": args.repetitions,
            "warmup_s": args.warmup,
            "duration_s": args.duration,
            "poller": args.poller or "default for the platform",
            "client_threads": args.client_threads,
            "server_cpus": args.server_cpus or "not pinned",
            "client_cpus": args.client_cpus or "not pinned",
        },
        "command": " ".join(sys.argv),
        "cells": cells,
        "skipped_for_open_files_limit": skipped,
        "discarded_runs": [],
        "finished": False,
    }

    def write_index():
        with open(os.path.join(args.directory, "index.json"), "w") as f:
            json.dump(index, f, indent=2)
            f.write("\n")

    write_index()

    for name in skipped:
        print(f"skipping {name}: needs more open files than the limit of {limit}")

    capacity = None

    for cell_config in cells:
        rate = 0
        if cell_config["rate_fraction"] is not None:
            rate = round(capacity * cell_config["rate_fraction"])

        throughputs = []

        for repetition in range(1, args.repetitions + 1):
            # A run taken while the machine slept or stalled is thrown
            # away and done again. After three tries it is kept, marked.
            for attempt in range(1, 4):
                run = run_once(args, cell_config, rate)

                if not run["suspect"]:
                    break

                index["discarded_runs"].append(
                    {"cell": cell_config["name"], "repetition": repetition}
                )
                write_index()
                print(
                    f"{cell_config['name']} [{repetition}/{args.repetitions}] "
                    f"discarded, the machine slept or stalled (try {attempt} of 3)",
                    flush=True,
                )

            run["repetition"] = repetition

            path = os.path.join(
                args.directory, "runs", f"{cell_config['name']}.rep{repetition}.json"
            )
            with open(path, "w") as f:
                json.dump(run, f, indent=1)
                f.write("\n")

            result = run["loadgen"]
            throughputs.append(result["throughput_rps"])

            def in_ms(name):
                value = result["latency_ns"][name]
                return f"{value / 1e6:8.3f} ms" if value else "     n/a"

            print(
                f"{cell_config['name']} [{repetition}/{args.repetitions}] "
                f"{result['throughput_rps']:>10.0f} req/s  "
                f"p50 {in_ms('p50')}  p99 {in_ms('p99')}"
                + ("  CLIENT SATURATED" if result["client"]["saturated"] else "")
                + ("  SUSPECT" if run["suspect"] else ""),
                flush=True,
            )

        if cell_config["name"].endswith("-capacity"):
            capacity = sorted(throughputs)[len(throughputs) // 2]

    index["finished"] = True
    write_index()
    print(f"\nresults in {os.path.relpath(args.directory)}")
    print(f"next: python3 benchmarks/analyze.py {os.path.relpath(args.directory)}")


if __name__ == "__main__":
    main()
