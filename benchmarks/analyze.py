#!/usr/bin/env python3
"""
Turns a results directory written by benchmarks/run.py into a report.

    python3 benchmarks/analyze.py benchmarks/results/<timestamp>-<suite>

It writes report.md and a few SVG plots into that directory. For every
cell it gives the median over the repetitions and the lowest and highest
repetition, because one run says nothing about how much runs differ.

Nothing here needs anything outside the Python standard library.
"""

import glob
import json
import math
import os
import statistics
import sys

COLORS = ["#2563eb", "#dc2626", "#059669", "#d97706", "#7c3aed", "#0891b2"]


def load(directory):
    with open(os.path.join(directory, "index.json")) as f:
        index = json.load(f)

    runs = {}
    for path in glob.glob(os.path.join(directory, "runs", "*.json")):
        with open(path) as f:
            run = json.load(f)
        runs.setdefault(run["cell"], []).append(run)

    for repetitions in runs.values():
        repetitions.sort(key=lambda run: run["repetition"])

    return index, runs


def median_min_max(values):
    values = [v for v in values if v is not None]
    if not values:
        return None
    return statistics.median(values), min(values), max(values)


def summarize(cell, repetitions):
    results = [run["loadgen"] for run in repetitions]
    servers = [run["server"] or {} for run in repetitions]

    def latency(name):
        # Left out unless every repetition had enough samples for it.
        values = [r["latency_ns"][name] for r in results]
        return None if None in values else statistics.median(values)

    def server(name):
        spread = median_min_max([s.get(name) for s in servers])
        return spread[0] if spread else None

    failed = sum(
        r["requests"][kind]
        for r in results
        for kind in ("busy", "errors", "timeouts", "connection_errors")
    )

    return {
        "cell": cell,
        "repetitions": len(repetitions),
        "throughput": median_min_max([r["throughput_rps"] for r in results]),
        "p50": latency("p50"),
        "p99": latency("p99"),
        "p999": latency("p999"),
        "max": max(r["latency_ns"]["max"] for r in results),
        "bytes_per_s": statistics.median(
            r["bytes"]["sent_per_s"] + r["bytes"]["received_per_s"] for r in results
        ),
        "failed": failed,
        "not_sent": sum(r["requests"]["not_sent"] for r in results),
        "connect_failures": sum(r["connections"]["connect_failures"] for r in results),
        "client_cpu": statistics.median(r["client"]["cpu_per_thread"] for r in results),
        "client_saturated": sum(1 for r in results if r["client"]["saturated"]),
        "suspect": sum(1 for run in repetitions if run["suspect"]),
        "mode": results[0]["config"]["mode"],
        "rate_target": results[0]["config"]["rate_target_rps"],
        "server_cpu": server("cpu_cores"),
        "worker_utilization": server("worker_utilization"),
        "rss": server("rss_bytes_max"),
        "queue_depth": max((s.get("queue_depth_max") or 0) for s in servers),
        "involuntary_switches": server("involuntary_switches_per_s"),
        "voluntary_switches": server("voluntary_switches_per_s"),
        "open_files": server("open_files_max"),
    }


def time_text(ns):
    if ns is None:
        return "n/a"
    if ns < 1e6:
        return f"{ns / 1e3:.0f} µs"
    if ns < 1e9:
        return f"{ns / 1e6:.2f} ms"
    return f"{ns / 1e9:.2f} s"


def number_text(value, digits=0):
    return "n/a" if value is None else f"{value:,.{digits}f}"


def nice_ticks(low, high, count=5):
    if high <= low:
        return [low]
    step = 10 ** math.floor(math.log10((high - low) / count))
    for factor in (1, 2, 5, 10):
        if (high - low) / (step * factor) <= count:
            step *= factor
            break
    first = math.floor(low / step) * step
    ticks = []
    value = first
    while value <= high + step * 0.001:
        ticks.append(round(value, 10))
        value += step
    return ticks


def short_number(value):
    if value >= 1e6:
        return f"{value / 1e6:g}M"
    if value >= 1e3:
        return f"{value / 1e3:g}k"
    return f"{value:g}"


def line_chart(path, title, x_label, y_label, series, x_ticks, log_x=False):
    """A small SVG line chart.

    series is a list of (name, points), each point (x, y, y_low, y_high).
    The bar through a point runs from the lowest to the highest
    repetition. x_ticks is a list of (x, label).
    """
    width, height = 720, 420
    left, right, top, bottom = 80, 190, 50, 60
    plot_w, plot_h = width - left - right, height - top - bottom

    def x_scale(x):
        return math.log10(x) if log_x else x

    xs = [x_scale(x) for x, _ in x_ticks]
    x_low, x_high = min(xs), max(xs)
    if x_high == x_low:
        x_high = x_low + 1

    y_high = max(p[3] for _, points in series for p in points) or 1
    ticks = nice_ticks(0, y_high)
    y_high = ticks[-1]

    def px(x):
        return left + (x_scale(x) - x_low) / (x_high - x_low) * plot_w

    def py(y):
        return top + plot_h - y / y_high * plot_h

    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}" font-family="sans-serif" font-size="12">',
        f'<rect width="{width}" height="{height}" fill="white"/>',
        f'<text x="{left}" y="28" font-size="15" font-weight="bold">{title}</text>',
    ]

    for tick in ticks:
        y = py(tick)
        out.append(
            f'<line x1="{left}" y1="{y:.1f}" x2="{left + plot_w}" y2="{y:.1f}" stroke="#e5e7eb"/>'
        )
        out.append(
            f'<text x="{left - 8}" y="{y + 4:.1f}" text-anchor="end">{short_number(tick)}</text>'
        )

    for x, label in x_ticks:
        out.append(
            f'<text x="{px(x):.1f}" y="{top + plot_h + 18}" text-anchor="middle">{label}</text>'
        )

    out.append(
        f'<rect x="{left}" y="{top}" width="{plot_w}" height="{plot_h}" fill="none" stroke="#9ca3af"/>'
    )
    out.append(
        f'<text x="{left + plot_w / 2}" y="{height - 16}" text-anchor="middle">{x_label}</text>'
    )
    out.append(
        f'<text transform="translate(18 {top + plot_h / 2}) rotate(-90)" text-anchor="middle">{y_label}</text>'
    )

    for number, (name, points) in enumerate(series):
        color = COLORS[number % len(COLORS)]
        path_points = " ".join(f"{px(x):.1f},{py(y):.1f}" for x, y, _, _ in points)
        out.append(
            f'<polyline points="{path_points}" fill="none" stroke="{color}" stroke-width="2"/>'
        )
        for x, y, low, high in points:
            out.append(
                f'<line x1="{px(x):.1f}" y1="{py(low):.1f}" x2="{px(x):.1f}" y2="{py(high):.1f}" '
                f'stroke="{color}" stroke-width="1.5"/>'
            )
            out.append(f'<circle cx="{px(x):.1f}" cy="{py(y):.1f}" r="3.5" fill="{color}"/>')

        legend_y = top + 12 + number * 20
        out.append(
            f'<line x1="{left + plot_w + 16}" y1="{legend_y}" x2="{left + plot_w + 40}" '
            f'y2="{legend_y}" stroke="{color}" stroke-width="2"/>'
        )
        out.append(f'<text x="{left + plot_w + 46}" y="{legend_y + 4}">{name}</text>')

    out.append("</svg>")

    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")


def throughput_points(cells, summaries, x_of):
    return [
        (x_of(c), *summaries[c["name"]]["throughput"])
        for c in cells
        if c["name"] in summaries
    ]


def plots(directory, index, summaries):
    """One plot per kind of suite. Returns [(file name, caption)]."""
    made = []
    groups = {}

    for c in index["cells"]:
        if c["name"] in summaries:
            groups.setdefault(c["group"], []).append(c)

    def save(name, caption, *args, **kwargs):
        line_chart(os.path.join(directory, name), *args, **kwargs)
        made.append((name, caption))

    if "connections" in groups:
        cells = groups["connections"]
        save(
            "throughput-by-connections.svg",
            "Throughput against the number of connections",
            "Echo of 64 bytes, closed loop",
            "connections (log scale)",
            "requests per second",
            [("median, bar from lowest to highest run",
              throughput_points(cells, summaries, lambda c: c["connections"]))],
            [(c["connections"], f"{c['connections']:,}") for c in cells],
            log_x=True,
        )

    if "payloads" in groups:
        cells = groups["payloads"]
        save(
            "throughput-by-payload.svg",
            "Throughput against the payload size",
            "Echo over 100 connections, closed loop",
            "payload in bytes (log scale)",
            "requests per second",
            [("median, bar from lowest to highest run",
              throughput_points(cells, summaries, lambda c: c["load"]["payload"]))],
            [(c["load"]["payload"], f"{c['load']['payload']:,}") for c in cells],
            log_x=True,
        )

    if "mix" in groups:
        cells = groups["mix"]
        save(
            "throughput-by-idle-connections.svg",
            "Throughput of 100 busy connections against idle ones beside them",
            "100 busy connections with idle ones beside them",
            "idle connections",
            "requests per second",
            [("median, bar from lowest to highest run",
              throughput_points(cells, summaries, lambda c: c["idle_connections"]))],
            [(c["idle_connections"], f"{c['idle_connections']:,}") for c in cells],
        )

    if "workers" in groups:
        cells = groups["workers"]
        workloads = []
        for c in cells:
            workload = c["name"].rsplit("-", 1)[0]
            if workload not in workloads:
                workloads.append(workload)

        # Each workload relative to its own result with one worker, so
        # that workloads of very different speed fit on one plot.
        series = []
        for workload in workloads:
            own = [c for c in cells if c["name"].rsplit("-", 1)[0] == workload]
            base = summaries[own[0]["name"]]["throughput"][0]
            series.append(
                (
                    workload,
                    [
                        (c["workers"], *(v / base for v in summaries[c["name"]]["throughput"]))
                        for c in own
                    ],
                )
            )

        worker_counts = sorted({c["workers"] for c in cells})
        series.append(("in step with the workers", [(w, w, w, w) for w in worker_counts]))

        save(
            "scaling-by-workers.svg",
            "Throughput against the number of workers, relative to one worker",
            "Throughput relative to one worker",
            "worker threads",
            "times the throughput of one worker",
            series,
            [(w, str(w)) for w in worker_counts],
        )

    if "latency" in groups:
        cells = [c for c in groups["latency"] if c["rate_fraction"] is not None]
        if cells:
            series = []
            for name, label in (("p50", "median"), ("p99", "99th percentile"), ("p999", "99.9th percentile")):
                points = [
                    (c["rate_fraction"] * 100, summaries[c["name"]][name] / 1e3)
                    for c in cells
                    if summaries[c["name"]][name] is not None
                ]
                if points:
                    series.append((label, [(x, y, y, y) for x, y in points]))

            save(
                "latency-by-load.svg",
                "Latency against the request rate, open loop",
                "Latency at a fixed request rate (open loop)",
                "request rate as a share of the measured capacity, in percent",
                "latency in microseconds",
                series,
                [(c["rate_fraction"] * 100, f"{int(c['rate_fraction'] * 100)}%") for c in cells],
            )

    return made


def report(directory, index, summaries, made):
    env = index["environment"]
    build = env["build"]
    settings = index["settings"]
    lines = []
    add = lines.append

    add(f"# Benchmark report: {index['suite']}")
    add("")
    add("Generated by `benchmarks/analyze.py` from the raw results in `runs/`.")
    add("")
    add("## Where these numbers come from")
    add("")
    add("| | |")
    add("| --- | --- |")
    add(f"| Date (UTC) | {env['date_utc']} |")
    add(f"| Machine | {env['cpu']}, {env['cores']} cores, {env['machine']} |")
    add(f"| OS | {env['os']} ({env['kernel']}) |")

    if env.get("power"):
        add(f"| Power | {env['power']} |")

    if env["in_container"]:
        add("| Container | Yes. Linux in a virtual machine, so not what a Linux machine would give. |")

    add(f"| Compiler | {build['compiler']} |")
    add(f"| Build | {build['build_type']}, flags `{build['flags'].strip()}`, sanitizer {build['sanitizer']} |")

    commit = env["git_commit"] or "unknown"
    if env["git_uncommitted_changes"]:
        commit += " plus uncommitted changes"
    add(f"| Commit | {commit} |")
    add(f"| Poller | {settings['poller']} |")
    add(
        f"| Each cell | {settings['repetitions']} runs of {settings['duration_s']:g} s "
        f"after {settings['warmup_s']:g} s of warm-up, fresh server each run |"
    )
    add(f"| Client threads | {settings['client_threads']} |")
    add(f"| CPU pinning | server: {settings['server_cpus']}, client: {settings['client_cpus']} |")
    add(f"| Command | `{index['command']}` |")
    add("")

    if not index["finished"]:
        add("**This suite did not run to the end.** What follows is what there is.")
        add("")

    if index.get("discarded_runs"):
        add(
            f"{len(index['discarded_runs'])} run(s) were thrown away and done again "
            "because the machine slept or stalled while they ran."
        )
        add("")

    if index["skipped_for_open_files_limit"]:
        add(
            "Not run, because the limit on open files is too low: "
            + ", ".join(index["skipped_for_open_files_limit"])
            + "."
        )
        add("")

    add("## Results")
    add("")
    add("Throughput is the median over the runs, with the lowest and highest run")
    add("beside it. Latencies are the median over the runs of each run's")
    add("percentile. A percentile is left out when a run had too few samples for it.")
    add("")
    add("| Cell | Requests/s | Lowest to highest | p50 | p99 | p99.9 | Worst | MiB/s | Not answered |")
    add("| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |")

    for c in index["cells"]:
        s = summaries.get(c["name"])
        if not s:
            continue
        median, low, high = s["throughput"]
        add(
            f"| {s['cell']} | {median:,.0f} | {low:,.0f} to {high:,.0f} | "
            f"{time_text(s['p50'])} | {time_text(s['p99'])} | {time_text(s['p999'])} | "
            f"{time_text(s['max'])} | {s['bytes_per_s'] / 1048576:.1f} | {s['failed']:,} |"
        )

    add("")
    add("Latency in a closed loop is counted from when a request was sent. In an")
    add("open loop it is counted from when the request was due, so it includes any")
    add("time the request waited to be sent.")
    add("")

    open_cells = [s for s in summaries.values() if s["mode"] == "open"]
    if open_cells:
        add("Open-loop cells and the rate they asked for:")
        add("")
        for s in open_cells:
            add(
                f"- {s['cell']}: asked for {s['rate_target']:,.0f} per second, "
                f"{s['not_sent']:,} requests were due and never sent."
            )
        add("")

    add("## The server while it was measured")
    add("")
    add("| Cell | CPU (cores) | Workers busy | Memory | Deepest queue | Switches/s forced | Switches/s by choice |")
    add("| --- | ---: | ---: | ---: | ---: | ---: | ---: |")

    for c in index["cells"]:
        s = summaries.get(c["name"])
        if not s:
            continue
        busy = "n/a" if s["worker_utilization"] is None else f"{s['worker_utilization'] * 100:.0f}%"
        memory = "n/a" if s["rss"] is None else f"{s['rss'] / 1048576:.0f} MiB"
        add(
            f"| {s['cell']} | {number_text(s['server_cpu'], 2)} | {busy} | {memory} | "
            f"{s['queue_depth']:,} | {number_text(s['involuntary_switches'])} | "
            f"{number_text(s['voluntary_switches'])} |"
        )

    add("")
    add("CPU is in cores: 1.00 is one core used all the time. Context switches")
    add("are only available on Linux.")
    add("")

    add("## Was the client the limit?")
    add("")
    saturated = [s for s in summaries.values() if s["client_saturated"]]

    if saturated:
        add("**Yes, in these cells.** Their numbers say what the client could do,")
        add("not what the server could do:")
        add("")
        for s in saturated:
            add(
                f"- {s['cell']}: {s['client_saturated']} of {s['repetitions']} runs, "
                f"client CPU per thread {s['client_cpu']:.2f}."
            )
    else:
        add("No run was flagged. In a closed loop a run is flagged when the client's")
        add("threads use 85% or more of their time. In an open loop it is flagged")
        add("when more than 1% of requests were sent over a millisecond late.")

    add("")
    add("| Cell | Client CPU per thread |")
    add("| --- | ---: |")
    for c in index["cells"]:
        s = summaries.get(c["name"])
        if s:
            add(f"| {s['cell']} | {s['client_cpu']:.2f} |")
    add("")

    problems = [s for s in summaries.values() if s["suspect"] or s["connect_failures"]]
    if problems:
        add("## Runs not to be trusted")
        add("")
        for s in problems:
            if s["suspect"]:
                add(
                    f"- {s['cell']}: {s['suspect']} run(s) took far longer by the wall clock "
                    "than they should have. The machine was asleep or stalled."
                )
            if s["connect_failures"]:
                add(f"- {s['cell']}: {s['connect_failures']:,} connections could not be made.")
        add("")

    if made:
        add("## Plots")
        add("")
        for name, caption in made:
            add(f"![{caption}]({name})")
            add("")

    with open(os.path.join(directory, "report.md"), "w") as f:
        f.write("\n".join(lines).rstrip("\n") + "\n")


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)

    directory = sys.argv[1]
    index, runs = load(directory)

    summaries = {cell: summarize(cell, repetitions) for cell, repetitions in runs.items()}
    made = plots(directory, index, summaries)
    report(directory, index, summaries, made)

    print(f"{'cell':34} {'req/s median':>13} {'lowest':>10} {'highest':>10} {'p50':>10} {'p99':>10}")
    for c in index["cells"]:
        s = summaries.get(c["name"])
        if not s:
            continue
        median, low, high = s["throughput"]
        print(
            f"{s['cell']:34} {median:13,.0f} {low:10,.0f} {high:10,.0f} "
            f"{time_text(s['p50']):>10} {time_text(s['p99']):>10}"
            + ("  client saturated" if s["client_saturated"] else "")
            + ("  SUSPECT" if s["suspect"] else "")
        )

    print(f"\nwrote {os.path.join(directory, 'report.md')}")
    for name, _ in made:
        print(f"wrote {os.path.join(directory, name)}")


if __name__ == "__main__":
    main()
