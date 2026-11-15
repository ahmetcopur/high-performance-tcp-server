# Soak test

**Passed.** 30.1 minutes in 30 cycles of 60 s, the first 3 left out as warm-up.

| Check | Result | |
| --- | --- | --- |
| descriptors at the end of every cycle are what they were at the start | pass | 7 throughout |
| memory at the end of the cycles does not grow | pass | first third 4.27 MiB, last third 4.39 MiB, fitted growth over the run +0.94 MiB, allowed 2.0 MiB |
| no well-behaved request failed | pass | none |
| every idle connection was closed by the idle timeout, and no other | pass | 6,000 closed |
| no request failed inside a worker | pass | 0 |
| the server stops with exit code 0 when asked | pass | exit code 0 |
| the machine did not sleep during the run | pass | it did not |

## Where this was run

| | |
| --- | --- |
| Date (UTC) | 2026-11-15T20:44:46+00:00 |
| Machine | Apple M1, 8 cores, arm64 |
| OS | macOS 26.6.2 (Darwin 25.6.0) |
| Power | Battery Power |
| Compiler | AppleClang 17.0.0.17000013 |
| Build | Release, flags `-O3 -DNDEBUG`, sanitizer none |
| Commit | cedb89ea7b63e5e57a8c821a2053307837f99ff4 plus uncommitted changes |
| Server | `build-release/server --port 18091 --workers 4 --poller poll --idle-timeout-ms 30000` |
| Command | `benchmarks/soak.py --build build-release --minutes 30` |

## What the server did

| Load | Requests answered |
| --- | ---: |
| echo | 59,221,763 |
| large | 16,204,688 |
| compute | 30,206,272 |
| ping | 33,448,866 |
| **all** | **139,081,589** |

By the server's own count it handed 106,217,046 requests to its workers, refused 0 for a full queue, and 0 failed in a worker. The misbehaving clients are in those numbers and not in the table.

The server wrote 30 lines to its log, which is `server.log` here. It reports timeouts and connection errors at most once a second each.

## The server at the end of each cycle

Idle, with every client gone. Peak is the most seen during the cycle.
Memory is resident memory plus what was swapped out on Linux, and the
footprint on macOS, which also counts memory the system has compressed.

![memory](memory.svg)

| Cycle | Minute | Memory | Descriptors | Peak memory | Peak descriptors |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 (warm-up) | 1.0 | 2.91 MiB | 7 | 3.6 MiB | 249 |
| 2 (warm-up) | 2.0 | 4.11 MiB | 7 | 4.1 MiB | 249 |
| 3 (warm-up) | 3.0 | 3.31 MiB | 7 | 4.3 MiB | 250 |
| 4 | 4.0 | 3.05 MiB | 7 | 3.6 MiB | 249 |
| 5 | 5.0 | 3.25 MiB | 7 | 3.7 MiB | 249 |
| 6 | 6.0 | 3.20 MiB | 7 | 4.3 MiB | 249 |
| 7 | 7.0 | 2.70 MiB | 7 | 3.4 MiB | 249 |
| 8 | 8.0 | 4.27 MiB | 7 | 4.3 MiB | 249 |
| 9 | 9.0 | 4.33 MiB | 7 | 4.3 MiB | 249 |
| 10 | 10.0 | 4.33 MiB | 7 | 4.3 MiB | 249 |
| 11 | 11.0 | 4.33 MiB | 7 | 4.3 MiB | 249 |
| 12 | 12.0 | 4.33 MiB | 7 | 4.3 MiB | 249 |
| 13 | 13.0 | 4.33 MiB | 7 | 4.3 MiB | 249 |
| 14 | 14.0 | 4.33 MiB | 7 | 4.3 MiB | 249 |
| 15 | 15.0 | 4.33 MiB | 7 | 4.3 MiB | 249 |
| 16 | 16.0 | 4.33 MiB | 7 | 4.3 MiB | 249 |
| 17 | 17.0 | 4.39 MiB | 7 | 4.4 MiB | 249 |
| 18 | 18.1 | 4.39 MiB | 7 | 4.4 MiB | 249 |
| 19 | 19.1 | 4.39 MiB | 7 | 4.4 MiB | 249 |
| 20 | 20.1 | 4.39 MiB | 7 | 4.4 MiB | 249 |
| 21 | 21.1 | 4.47 MiB | 7 | 4.5 MiB | 250 |
| 22 | 22.1 | 4.47 MiB | 7 | 4.5 MiB | 249 |
| 23 | 23.1 | 3.13 MiB | 7 | 4.5 MiB | 249 |
| 24 | 24.1 | 4.33 MiB | 7 | 4.3 MiB | 249 |
| 25 | 25.1 | 4.39 MiB | 7 | 4.4 MiB | 249 |
| 26 | 26.1 | 4.39 MiB | 7 | 4.4 MiB | 249 |
| 27 | 27.1 | 4.39 MiB | 7 | 4.4 MiB | 249 |
| 28 | 28.1 | 4.45 MiB | 7 | 4.5 MiB | 249 |
| 29 | 29.1 | 4.45 MiB | 7 | 4.5 MiB | 249 |
| 30 | 30.1 | 4.45 MiB | 7 | 4.5 MiB | 249 |

Before the first cycle: 0.97 MiB, 7 descriptors.
