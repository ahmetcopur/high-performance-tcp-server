# Benchmark results, 15 November 2026

What the measurements show, with the evidence for each claim. The method
is in [README.md](README.md). Every number below is the median of five
runs and comes from a report under `results/`, which names the command
that produced it.

## Where the numbers come from

Two environments, on one machine. Numbers from one are not comparable
with numbers from the other.

| | Mac | Linux container |
| --- | --- | --- |
| Machine | Apple M1, 8 cores (4 fast, 4 slow) | The same machine, through Docker |
| OS | macOS 26.6.2 | Ubuntu 24.04.5 in a virtual machine, 8 CPUs |
| Compiler | Apple clang 17.0.0 | g++ 13.3.0 |
| Build | Release, `-O3 -DNDEBUG` | Release, `-O3 -DNDEBUG` |
| Server waits with | `poll()` | epoll, and `poll()` where it says so |
| Client waits with | `poll()` | epoll |
| CPU pinning | none, macOS has no way to | server on CPUs 0 to 3, client on 4 to 7 |
| Client threads | 4, and 2 in the latency runs | 4, and 2 in the latency runs |
| Power | battery, mains for the last latency run | not recorded in the container |
| Server code | commit `dafec02` | commit `dafec02` |

The server and the load generator are exactly as in commit `dafec02`.
The Mac reports say "plus uncommitted changes": those were to the runner
and the smoke script, which were being fixed between runs. The container
has no git and cannot tell.

Client and server share the machine and talk over loopback. The Linux
numbers show how the Linux code behaves. They are not what a Linux
machine would give.

Unless it says otherwise, a cell is echo of 64 bytes over 100
connections with 4 workers, in a closed loop.

## 1. For cheap requests, the event loop thread is the limit

Echo of 64 bytes reaches about 100,000 to 130,000 requests per second
and stops there. While it does, the server uses a little over one core
and its workers are almost idle.

| | Requests/s | Server CPU | Workers busy |
| --- | ---: | ---: | ---: |
| Mac, 100 connections | 122,046 | 1.36 cores | 2.5% |
| Linux, 100 connections | 112,807 | 1.14 cores | 1.4% |

One thread reads, decodes, hands over, collects and writes every
request. Adding workers cannot help while that thread is full.

Reports: [Mac](results/20261115T074842Z-connections-macos-poll/report.md),
[Linux](results/20261115T081040Z-connections-linux-epoll/report.md).

## 2. Workers: more is not better

The worker-scaling experiment, 100 connections, 1, 2, 4 and 8 workers.
"Light" compute is 100 passes over 64 bytes, a few microseconds. "Heavy"
is 10,000 passes over 1 KiB, about 13 ms.

Linux, server pinned to 4 CPUs:

| Workload | 1 worker | 2 workers | 4 workers | 8 workers |
| --- | ---: | ---: | ---: | ---: |
| Echo, requests/s | 213,639 | 128,064 | 99,541 | 102,193 |
| workers busy | 6% | 3% | 1% | 1% |
| switches by choice, per second | 23,323 | 44,662 | 66,268 | 88,725 |
| Light compute, requests/s | 103,264 | 213,497 | 161,972 | 138,541 |
| workers busy | 99% | 96% | 39% | 17% |
| switches by choice, per second | 17,762 | 2,166 | 34,154 | 61,038 |
| Heavy compute, requests/s | 76 | 150 | 289 | 289 |
| server CPU, cores | 1.00 | 2.00 | 4.00 | 4.00 |

Mac, nothing pinned:

| Workload | 1 worker | 2 workers | 4 workers | 8 workers |
| --- | ---: | ---: | ---: | ---: |
| Echo, requests/s | 130,807 | 118,626 | 106,829 | 95,912 |
| Light compute, requests/s | 88,232 | 119,499 | 96,297 | 94,398 |
| Heavy compute, requests/s | 75 | 147 | 286 | 441 |
| server CPU, cores | 1.00 | 1.99 | 4.05 | 7.25 |

What this shows:

- **Heavy compute follows the cores it has.** Doubling the workers
  doubles the throughput, until the cores run out. On Linux the server
  was pinned to four CPUs, so eight workers do what four do. On the Mac
  eight workers give 5.9 times one worker and not 8, on a machine where
  four of the eight cores are slow ones.
- **Echo is fastest with one worker,** on both systems. On Linux a
  second worker costs 40% of the throughput, and four or eight cost half.
  On the Mac each doubling costs about 10%. The work per request is too
  small for a second thread to help, and the server's CPU use stays near
  one core throughout.
- **Light compute is fastest with two workers,** not one and not eight,
  on both systems. With one worker, the worker is the limit (99% busy).
  With two, they are still nearly always busy. With more, each sits idle
  most of the time.
- **On Linux, with two workers or more, light compute is faster than
  echo:** 213,497 against 128,064 with two, 161,972 against 99,541 with
  four. Doing more work per request made the server faster. The Mac does
  not show this: there the two are level with two and eight workers, and
  echo is ahead with four.

The likely cause is the cost of waking a sleeping worker. A worker with
nothing to do sleeps, and handing it a request wakes it, which is a
system call and a context switch. A worker that is still busy when the
next request arrives needs no waking. With one worker, a burst of
requests wakes it once and it works through all of them. With eight, the
same burst wakes several.

The Linux context switches fit this. On echo, the server's threads give
up the CPU 23,323 times a second with one worker and 88,725 times with
eight, and throughput halves. On light compute with two workers, both
nearly always busy, it is 2,166 times, the lowest figure in the table,
and the throughput equals the best in the table.

This is an inference from counters, not a measurement of the wake-up
itself, and there are no such counters for the Mac. Step 10 profiles it.

Reports: [Linux](results/20261115T082049Z-workers-linux-epoll/report.md),
[Mac](results/20261115T075857Z-workers-macos-poll/report.md).

## 3. Idle connections: what epoll is for

100 busy connections, with idle connections open beside them.

| Idle connections | Linux, epoll | Linux, `poll()` | Mac, `poll()` |
| ---: | ---: | ---: | ---: |
| 0 | 101,900 | 117,207 | 117,223 |
| 1,000 | 102,390 | 93,854 | 32,676 |
| 5,000 | 99,747 | 39,424 | 6,085 |

With epoll, 5,000 idle connections cost nothing. With `poll()` they cost
two thirds of the throughput on Linux and nineteen twentieths on the
Mac. `poll()` is handed every descriptor on every call and looks at each
one, so a wait costs the same whether a connection is busy or idle.
epoll hands back only the ones that are ready.

The Mac column is worse than the Linux `poll()` column partly because on
the Mac the client waits with `poll()` too. In the run with 5,000 idle
connections the client was flagged as the limit in 4 of 5 runs. On Linux
the client used epoll in both columns, so that comparison isolates the
server.

Reports:
[Linux epoll](results/20261115T081901Z-mix-linux-epoll/report.md),
[Linux poll](results/20261115T083746Z-mix-linux-poll/report.md),
[Mac](results/20261115T075709Z-mix-macos-poll/report.md).

## 4. Busy connections: `poll()` and epoll are level

Echo of 64 bytes with every connection sending.

| Connections | Linux, epoll | Linux, `poll()` | Mac, `poll()` |
| ---: | ---: | ---: | ---: |
| 1 | 13,537 | 13,856 | 32,300 |
| 10 | 73,401 | 72,977 | 103,134 |
| 100 | 112,807 | 112,007 | 122,046 |
| 1,000 | 142,479 | 157,277 | 104,476 |
| 5,000 | 85,693 | 82,493 | 84,219 |
| 10,000 | 112,420 | 103,833 | 77,953 |

When every connection is busy, each call to `poll()` comes back with
many connections ready, so the cost of looking at all of them is spread
over many requests. The two pollers are within the spread of the runs.
The difference between them is in section 3.

The spread is wide here. On Linux at 5,000 connections the five runs
range from 78,984 to 101,192, and 5,000 comes out lower than 10,000,
which has no good explanation and should not be read as a fact about
the server. On the Mac the client was flagged as the limit from 1,000
connections up.

Some runs with thousands of connections had requests answered "server
busy", because 10,000 requests in flight can fill a queue of 1,024. At
10,000 connections: one run in five on the Mac (739 requests), three on
Linux with epoll (379 to 2,246) and two with `poll()` (612 and 1,521).
At 5,000 connections: one run on Linux with `poll()` (971). Those
requests are not counted in the throughput.

Reports:
[Linux epoll](results/20261115T081040Z-connections-linux-epoll/report.md),
[Linux poll](results/20261115T083934Z-connections-linux-poll/report.md),
[Mac](results/20261115T074842Z-connections-macos-poll/report.md).

## 5. Payload size

Echo over 100 connections.

| Payload | Mac, requests/s | Mac, MiB/s both ways | Linux, requests/s |
| ---: | ---: | ---: | ---: |
| 64 B | 121,232 | 18 | 104,177 |
| 256 B | 119,064 | 61 | 110,511 |
| 1 KiB | 113,659 | 225 | 98,919 |
| 4 KiB | 109,494 | 858 | 78,631 |
| 64 KiB | 30,491 | 3,812 | 14,774 |

On the Mac the request rate falls by a tenth between 64 bytes and
4 KiB, so up to there the cost of a request is mostly not its bytes. On
Linux it falls by a quarter. At 64 KiB the bytes dominate: the Mac moves
3.7 GiB per second through loopback, counting both directions, and Linux
1.8 GiB.

Reports: [Mac](results/20261115T075223Z-payloads-macos-poll/report.md),
[Linux](results/20261115T081415Z-payloads-linux-epoll/report.md).

## 6. Latency: a closed loop measures the queue, not the server

Linux, two client threads. The first row is a closed loop. The others
send at a fixed rate, a share of what the closed loop reached, and count
latency from when each request was due.

| | Requests/s | Median | 99th percentile | 99.9th |
| --- | ---: | ---: | ---: | ---: |
| Closed loop, 100 connections | 92,577 | 1.06 ms | 1.78 ms | 2.26 ms |
| Fixed rate, 25% | 23,150 | 49 µs | 113 µs | 137 µs |
| Fixed rate, 50% | 46,290 | 49 µs | 124 µs | 156 µs |
| Fixed rate, 75% | 69,438 | 143 µs | 313 µs | 362 µs |
| Fixed rate, 90% | 83,325 | 196 µs | 412 µs | 494 µs |

The closed loop's median of 1.06 ms is 100 requests in flight divided by
92,577 per second. It is the time a request waits behind 99 others, and
says little about the server. At half the load a request takes 49 µs.

At every fixed rate the client kept its schedule: no request was sent
late and none was left unsent.

**On the Mac the open loop cannot be trusted above 25%.** With nothing
pinned, the client's threads were themselves held up, and the latencies
show that delay and not the server's. With four client threads the
client ran up to 110 ms behind its own schedule, and runs were flagged
at every rate. With two client threads, at 25% the median was 216 µs and
the 99th percentile 643 µs, and no run was flagged. At 50%, 75% and 90%,
three, two and one run in five were flagged, the client was up to 55 ms
behind, and the 99th percentile of 36 ms and more in those cells is the
client's.

Reports:
[Linux](results/20261115T082757Z-latency-linux-epoll/report.md),
[Mac, two client threads](results/20261115T084310Z-latency-macos-poll/report.md),
[Mac, four client threads](results/20261115T080606Z-latency-macos-poll/report.md).

## 7. Compute at three costs

100 connections, 4 workers.

| | Mac, requests/s | Linux, requests/s | Workers busy |
| --- | ---: | ---: | ---: |
| Light: 100 passes over 64 B | 110,779 | 160,595 | 36% and 38% |
| Medium: 1,000 passes over 1 KiB | 2,758 | 2,855 | 100% |
| Heavy: 10,000 passes over 1 KiB | 286 | 289 | 100% |

Medium and heavy are limited by the four workers and agree between the
two systems to within 4%. That is what it should look like when the
work is the same arithmetic on the same CPU.

Reports: [Mac](results/20261115T075521Z-compute-macos-poll/report.md),
[Linux](results/20261115T081714Z-compute-linux-epoll/report.md).

## What was thrown away

- **A whole first run on the Mac.** The lid was closed two minutes after
  it started and the machine slept through it. The runner now compares
  the wall clock with the monotonic clock and redoes any run taken while
  the machine slept. None had to be redone in the runs above.
- **Two Linux runs with `--poller poll`.** The option had been passed to
  the client as well, so both sides were slowed and the comparison did
  not isolate the server. The runner no longer passes it to the client,
  and both suites were run again.

## What these numbers are not

- Not from a network. Loopback has no latency, no loss and no limit on
  bandwidth worth mentioning.
- Not from a server machine. A laptop on battery, with fast and slow
  cores, and for Linux a virtual machine on top.
- Not a comparison with any other server.
- Not stable to the percent. The spread between runs is given in every
  report and is often 5% to 15%.
