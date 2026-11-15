#!/usr/bin/env python3
"""
Profiles the server while the load generator drives it. Linux only. It
needs perf, which the image from Dockerfile.profile has, and a container
started with --privileged.

Usage:
    python3 benchmarks/profile.py --build /build --tool cpu
    python3 benchmarks/profile.py --build /build --tool syscalls --load light --workers 2

Tools:
    cpu        perf samples the server's threads on a clock. Says where
               the CPU time goes, and draws a flame graph.
    syscalls   perf counts the server's system calls at the kernel's
               tracepoints.
    heap       perf stops at every malloc() and notes who called it.
               Slows the server down, so only the count per request means
               anything.

Everything goes to benchmarks/profiles/<time>-<tool>-<load>-<N>w/, with
the environment and the commands beside it.

Build with frame pointers, or perf cannot tell who called what:
    cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release \\
          -DCMAKE_CXX_FLAGS="-g -fno-omit-frame-pointer"
"""

import argparse
import collections
import datetime
import json
import os
import re
import signal
import socket
import subprocess
import sys
import time

import flamegraph
import run
from run import protocol

LOADS = {"echo": run.echo(64), "light": run.LIGHT, "heavy": run.HEAVY}

# What the server calls in libc to reach the kernel, or to wait. A sample
# is filed under the first of these found on its stack, looking from the
# innermost frame outwards. Anything else is the server's own code.
CALLS = {
    "epoll_wait": "epoll_wait",
    "epoll_pwait": "epoll_wait",
    "epoll_ctl": "epoll_ctl",
    "recv": "recv",
    "__recv": "recv",
    "send": "send",
    "__send": "send",
    "read": "read (waker)",
    "__read": "read (waker)",
    "write": "write (waker)",
    "__write": "write (waker)",
    "pthread_cond_signal": "pthread_cond_signal (wake a worker)",
    "pthread_cond_wait": "pthread_cond_wait (worker goes to sleep)",
    "std::condition_variable::wait(std::unique_lock<std::mutex>&)":
        "pthread_cond_wait (worker goes to sleep)",
    "pthread_mutex_lock": "mutex",
    "pthread_mutex_unlock": "mutex",
    "__lll_lock_wait": "mutex",
    "__lll_lock_wake": "mutex",
    "malloc": "malloc and free",
    "free": "malloc and free",
    "cfree": "malloc and free",
    "operator new(unsigned long)": "malloc and free",
    "operator delete(void*)": "malloc and free",
    "operator delete(void*, unsigned long)": "malloc and free",
    "__kernel_clock_gettime": "clock_gettime",
    "clock_gettime": "clock_gettime",
    "std::chrono::_V2::steady_clock::now()": "clock_gettime",
}


def stats(port):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        return protocol.parse_stats(protocol.request(sock, protocol.STATS, 1))


class Window:
    # The stretch of time a tool measures, and how many requests the
    # server took in during it. The two readings are taken just outside
    # the stretch, so figures per request come out a percent or two low.
    def open(self):
        self.before = stats(run.PORT)
        self.started = time.monotonic()

    def close(self):
        self.seconds = time.monotonic() - self.started
        self.after = stats(run.PORT)
        self.requests = self.after["tasks_submitted"] - self.before["tasks_submitted"]


def short(symbol):
    # Template and argument lists make the names too long to read.
    symbol = re.sub(r"std::chrono::time_point<.*?> > >", "time_point", symbol)
    symbol = symbol.replace(
        "std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >",
        "std::string",
    )
    return symbol


def read_samples(perf_data, server_pid):
    # Yields (role, frames from the outermost to the innermost). A frame
    # is (function, file it is in).
    script = subprocess.run(
        ["perf", "script", "-i", perf_data, "-F", "pid,tid,ip,sym,dso"],
        capture_output=True, text=True, check=True,
    ).stdout

    for block in script.split("\n\n"):
        lines = [line for line in block.splitlines() if line.strip()]
        if len(lines) < 2:
            continue

        tid = int(lines[0].split("/")[1])
        role = "event loop" if tid == server_pid else "worker"
        frames = []

        for line in lines[1:]:
            _, _, rest = line.strip().partition(" ")
            symbol, _, dso = rest.rpartition(" (")
            dso = os.path.basename(dso.rstrip(")"))

            if symbol == "[unknown]":
                symbol = f"[{dso}]"
            if dso == "[kernel.kallsyms]":
                symbol += "_[k]"

            frames.append((short(symbol), dso))

        frames.reverse()
        yield role, frames


def category(names):
    for name in reversed(names):
        if name in CALLS:
            return CALLS[name]
    return "the server's own code"


def profile_cpu(directory, server_pid, seconds, title, window):
    perf_data = os.path.join(directory, "perf.data")
    frequency = 1999

    window.open()
    subprocess.run(
        ["perf", "record", "-e", "cpu-clock", "-F", str(frequency), "-g",
         "-p", str(server_pid), "-o", perf_data, "--", "sleep", str(seconds)],
        check=True, capture_output=True,
    )
    window.close()

    folded = collections.Counter()
    by_category = {"event loop": collections.Counter(), "worker": collections.Counter()}
    own_code = {"event loop": collections.Counter(), "worker": collections.Counter()}
    in_kernel = collections.Counter()

    for role, frames in read_samples(perf_data, server_pid):
        names = [name for name, _ in frames]
        folded[";".join([role] + names)] += 1
        kind = category(names)
        by_category[role][kind] += 1

        if names[-1].endswith("_[k]"):
            in_kernel[role] += 1
        if kind == "the server's own code":
            own_code[role][names[-1]] += 1

    with open(os.path.join(directory, "stacks.folded"), "w") as f:
        for stack, count in sorted(folded.items()):
            f.write(f"{stack} {count}\n")

    with open(os.path.join(directory, "flame.svg"), "w") as f:
        f.write(flamegraph.draw(
            flamegraph.read_folded(os.path.join(directory, "stacks.folded")), title
        ))

    # The samples are the evidence and are kept as text. perf.data is
    # large, and only perf on this machine can read it.
    os.remove(perf_data)

    result = {"frequency_hz": frequency, "seconds": seconds, "threads": {}}

    for role in ("event loop", "worker"):
        samples = sum(by_category[role].values())
        result["threads"][role] = {
            "samples": samples,
            # One thread on the CPU all the time gives `frequency` samples
            # a second.
            "cores": round(samples / frequency / seconds, 3),
            "in_kernel": round(in_kernel[role] / samples, 4) if samples else None,
            "by_call": dict(by_category[role].most_common()),
            "own_code_top": dict(own_code[role].most_common(15)),
        }

    return result


# The system calls the server is expected to make. Everything else is
# counted together, from the tracepoint that sees every system call.
SYSCALLS = ("epoll_pwait", "recvfrom", "sendto", "epoll_ctl", "futex",
            "write", "read", "accept", "close", "shutdown")


def profile_syscalls(directory, server_pid, seconds, window):
    # strace -c was tried first. It stops the server at every system
    # call, which made it thirty times slower, and a server that slow
    # handles its connections in much larger batches than usual. The
    # calls made once per batch then look rarer than they are.
    # Tracepoints only count.
    table = os.path.join(directory, "perf-stat.csv")
    events = ["raw_syscalls:sys_enter"] + [f"syscalls:sys_enter_{name}" for name in SYSCALLS]
    command = ["perf", "stat", "-x", ",", "-o", table, "-p", str(server_pid)]
    for event in events:
        command += ["-e", event]

    window.open()
    subprocess.run(command + ["--", "sleep", str(seconds)], check=True, capture_output=True)
    window.close()

    calls = {}
    with open(table) as f:
        for line in f:
            fields = line.strip().split(",")
            if len(fields) > 2 and fields[0].isdigit():
                calls[fields[2].split("sys_enter_")[-1]] = int(fields[0])

    total = calls.pop("raw_syscalls:sys_enter")
    calls["other"] = total - sum(calls.values())

    return {"calls": dict(sorted(calls.items(), key=lambda item: -item[1]))}


def profile_heap(directory, server_pid, seconds, window):
    # heaptrack was tried first. It counted the allocations but could not
    # say where they came from: on this platform every call site came out
    # as "??". A probe on malloc() gives perf the whole stack.
    with open(f"/proc/{server_pid}/maps") as f:
        libc = next(line.split()[-1] for line in f if "/libc.so" in line)

    subprocess.run(["perf", "probe", "-q", "-d", "probe_libc:*"], capture_output=True)
    subprocess.run(["perf", "probe", "-q", "-x", libc, "malloc"], check=True, capture_output=True)

    # Two things at once. perf record keeps the stack of each call, but
    # at hundreds of thousands a second it drops some. perf stat only
    # counts, and misses none. The count comes from the second and the
    # shares of the callers from the first.
    perf_data = os.path.join(directory, "malloc.data")
    window.open()
    recorder = subprocess.Popen(
        ["perf", "record", "-e", "probe_libc:malloc", "-g", "-p", str(server_pid),
         "-o", perf_data, "--", "sleep", str(seconds)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    counted = subprocess.run(
        ["perf", "stat", "-x", ",", "-e", "probe_libc:malloc", "-p", str(server_pid),
         "--", "sleep", str(seconds)],
        check=True, capture_output=True, text=True,
    ).stderr
    # Closed here and not after the recorder has finished: it takes a
    # while to write its file, and counts nothing while it does.
    window.close()
    recorder.wait()
    total = int(counted.strip().splitlines()[-1].split(",")[0])

    # An allocation is filed under the innermost function of the server
    # itself, and the one that called it, which is usually what tells two
    # uses of the same helper apart.
    sites = collections.Counter()

    for role, frames in read_samples(perf_data, server_pid):
        own = [name for name, dso in frames if dso == "server"]
        site = " <- ".join(reversed(own[-2:])) if own else "outside the server's code"
        sites[f"{role}: {site}"] += 1

    os.remove(perf_data)
    subprocess.run(["perf", "probe", "-q", "-d", "probe_libc:*"], capture_output=True)

    recorded = sum(sites.values())

    return {
        "allocations": total,
        "stacks_recorded": recorded,
        "share_by_site": {site: round(count / recorded, 4) for site, count in sites.most_common()},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("--build", required=True)
    parser.add_argument("--tool", required=True, choices=("cpu", "syscalls", "heap"))
    parser.add_argument("--load", default="echo", choices=sorted(LOADS))
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--connections", type=int, default=100)
    parser.add_argument("--seconds", type=float, default=5.0)
    parser.add_argument("--server-cpus", default="0-3")
    parser.add_argument("--client-cpus", default="4-7")
    parser.add_argument("--server-option", action="append", default=[],
                        help="passed on to the server, e.g. --server-option=--queue-capacity=64")
    parser.add_argument("--results", default=os.path.join(run.HERE, "profiles"))
    args = parser.parse_args()

    if not sys.platform.startswith("linux"):
        raise SystemExit("perf and heaptrack are Linux tools")

    load = LOADS[args.load]
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    name = f"{stamp}-{args.tool}-{args.load}-{args.workers}w"
    directory = os.path.join(args.results, name)
    os.makedirs(directory)

    server_command = [
        os.path.join(args.build, "server"),
        "--port", str(run.PORT),
        "--workers", str(args.workers),
        "--idle-timeout-ms", "600000",
    ]
    for option in args.server_option:
        server_command += option.split("=", 1)

    loadgen_command = run.pinned(
        [
            os.path.join(args.build, "loadgen"),
            "--port", str(run.PORT),
            "--connections", str(args.connections),
            "--threads", "4",
            # The tool watches a window in the middle of the run.
            "--warmup", "1",
            "--duration", str(args.seconds + 2),
            "--type", load["type"],
            "--payload", str(load["payload"]),
            "--iterations", str(load.get("iterations", 0)),
            "--output", os.path.join(directory, "loadgen.json"),
        ],
        args.client_cpus,
    )

    server = subprocess.Popen(run.pinned(server_command, args.server_cpus),
                              stdout=subprocess.DEVNULL)
    run.wait_until_listening(server)

    # With taskset in front, the process started here became the server.
    server_pid = server.pid

    loadgen = subprocess.Popen(loadgen_command, stdout=subprocess.DEVNULL)
    time.sleep(2)

    window = Window()

    if args.tool == "cpu":
        title = (f"Server under {args.load}, {args.connections} connections, "
                 f"{args.workers} workers")
        result = profile_cpu(directory, server_pid, args.seconds, title, window)
    elif args.tool == "syscalls":
        result = profile_syscalls(directory, server_pid, args.seconds, window)
    else:
        result = profile_heap(directory, server_pid, args.seconds, window)

    loadgen.wait()
    os.kill(server_pid, signal.SIGINT)
    server.wait()

    requests = window.requests

    with open(os.path.join(directory, "loadgen.json")) as f:
        loadgen_result = json.load(f)

    result.update({
        "requests_in_window": requests,
        "window_s": round(window.seconds, 3),
        "requests_per_s_in_window": round(requests / window.seconds),
    })

    if args.tool == "syscalls":
        result["calls_per_request"] = {
            call: round(count / requests, 3) for call, count in result["calls"].items()
        }
        result["total_per_request"] = round(sum(result["calls"].values()) / requests, 2)

    if args.tool == "heap":
        result["allocations_per_request"] = round(result["allocations"] / requests, 2)
        result["by_site_per_request"] = {
            site: round(share * result["allocations"] / requests, 3)
            for site, share in result["share_by_site"].items()
        }

    profile = {
        "tool": args.tool,
        "load": {"name": args.load, **load},
        "workers": args.workers,
        "connections": args.connections,
        "environment": run.environment(args.build),
        "command": " ".join(["benchmarks/profile.py"] + sys.argv[1:]),
        "server_command": server_command,
        "loadgen_command": loadgen_command,
        "loadgen_throughput_rps": loadgen_result["throughput_rps"],
        "result": result,
    }

    with open(os.path.join(directory, "profile.json"), "w") as f:
        json.dump(profile, f, indent=1)
        f.write("\n")

    os.remove(os.path.join(directory, "loadgen.json"))

    print(directory)
    print(f"load generator: {loadgen_result['throughput_rps']:,.0f} requests/s")

    if args.tool == "cpu":
        for role, thread in result["threads"].items():
            print(f"\n{role}: {thread['cores']:.2f} cores, "
                  f"{(thread['in_kernel'] or 0) * 100:.0f}% of it in the kernel")
            for kind, count in thread["by_call"].items():
                print(f"  {count / thread['samples'] * 100:5.1f}%  {kind}")
    elif args.tool == "syscalls":
        print(f"\n{result['total_per_request']} system calls per request "
              f"({requests:,} requests)")
        for call, count in result["calls_per_request"].items():
            if count >= 0.001:
                print(f"  {count:6.3f}  {call}")
    else:
        print(f"\n{result['allocations_per_request']} allocations per request "
              f"({requests:,} requests)")
        for site, count in result["by_site_per_request"].items():
            if count >= 0.01:
                print(f"  {count:6.2f}  {site}")


if __name__ == "__main__":
    main()
