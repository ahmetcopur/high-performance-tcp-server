# Benchmarks

How the server is measured, and how to get any number in a report again.

| | |
| --- | --- |
| `loadgen/` | The load generator, a C++ client for the protocol. |
| `run.py` | Runs a suite: starts servers, drives the load generator, keeps every result. |
| `analyze.py` | Turns a results directory into `report.md` and plots. |
| `benchmark.py` | A quick smoke test in Python. Not for numbers. |
| `results/` | One directory per suite run, with the raw data. |

## Running it

Build a Release build first. The runner refuses anything else, because a
debug or sanitizer build measures the debugging, not the server.

```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build-release --parallel
```

```bash
python3 benchmarks/run.py --build build-release --suite connections
```

```bash
python3 benchmarks/analyze.py benchmarks/results/<the directory it printed>
```

Suites are `connections`, `payloads`, `compute`, `mix`, `workers`,
`latency`, `full` (all of those) and `quick` (a few seconds, to check
that everything works). `python3 benchmarks/run.py --help` describes
them.

Every report starts with the command that produced it, so any number can
be had again by running that command on the same commit.

### On Linux, from a Mac

The epoll backend exists only on Linux. This runs a suite in the
project's container, with server and client on separate CPUs:

```bash
docker run --rm -v "$PWD":/src -e TCPSERVER_COMMIT="$(git rev-parse HEAD)" tcpserver-dev sh -c "cmake -S /src -B /build -DCMAKE_BUILD_TYPE=Release && cmake --build /build --parallel && python3 /src/benchmarks/run.py --build /build --suite connections --server-cpus 0-3 --client-cpus 4-7"
```

Docker on a Mac runs Linux in a virtual machine. Its numbers show how
the Linux code behaves, and are not what a Linux machine would give. The
report says when it was made in a container.

## What one measurement is

A cell is one combination of workload, connections and workers. For each
cell the runner does this five times:

1. Start a fresh server.
2. Start the load generator. It opens every connection first, so that
   connection setup is not timed.
3. Run for 2 seconds and throw the results away (warm-up).
4. Measure for 5 seconds.
5. Stop the server.

The report gives the median of the five and the lowest and highest
beside it. One run says nothing about how much runs differ.

## Closed loop and open loop

**Closed loop** is the default. Each connection sends a request, waits
for the answer, and sends the next. This finds the highest throughput.
Its latencies flatter the server: when the server slows down the client
sends less, so slow moments are sampled less often.

**Open loop** (`--rate`) sends requests on a fixed schedule whether or
not the server has caught up, the way independent clients do. Latency is
counted from when a request was due, not from when it was finally sent.
A request that had to wait behind a slow one carries that wait.

Leaving that wait out is called coordinated omission. It is the usual
reason a benchmark reports a 99th percentile that no user ever sees. The
`latency` suite measures what the server can do in a closed loop, then
runs an open loop at 25%, 50%, 75% and 90% of that.

## What is recorded

For every repetition, in `results/<run>/runs/`:

- **From the load generator:** requests completed, answered "busy",
  answered wrongly, timed out, lost with their connection, unfinished at
  the end, and due but never sent. Bytes sent and received, counted at
  the socket. The latency histogram, complete. Its own CPU time.
- **From the server, four times a second:** CPU time, memory, and on
  Linux context switches and open files. From STATS: queue depth,
  connections, and the time workers spent on requests.
- **From the runner:** both command lines and how long the run took by
  the wall clock.

Once per suite, in `index.json`: machine, OS, kernel, compiler, flags,
build type, commit, power source, and the settings.

Percentiles are worked out from a histogram with logarithmic buckets
that keeps every value to within 0.8% of itself. A percentile is left
out when there are fewer than ten samples above it: with 500 samples the
99.9th percentile is one request, and one request is noise.

## Reading a report with suspicion

**Was the client the limit?** The load generator shares the machine with
the server. In a closed loop a run is flagged when the client's threads
are busy 85% of the time or more. In an open loop it is flagged when
more than 1% of requests were sent over a millisecond late. A flagged
cell says what the client could do.

**Did the machine sleep?** The runner compares the wall clock with the
monotonic clock, which stops during sleep. A run where they disagree is
thrown away and done again, and the report says how many were.

**What it cannot tell you:**

- Client and server run on one machine, over loopback. There is no
  network, no packet loss, and they compete for the same cores.
- On macOS nothing can be pinned to a CPU, and the machine has fast and
  slow cores that the scheduler moves threads between.
- On macOS the load generator itself waits with `poll()`, which gets
  slower with every connection. At thousands of connections the client
  is part of what is measured.
- One machine, one day. Numbers from different machines, or from the
  same machine on battery and on mains, are not comparable.
