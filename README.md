# TCP Server

[![CI](https://github.com/ahmetcopur/high-performance-tcp-server/actions/workflows/ci.yml/badge.svg)](https://github.com/ahmetcopur/high-performance-tcp-server/actions/workflows/ci.yml)

A concurrent TCP server in C++17 on POSIX, written from the socket calls
up, with no networking library. One event loop thread owns every socket
and hands the requests that cost CPU time to a pool of workers. It runs
on Linux with epoll and on macOS with `poll()`.

It is a study project. The point was to build each part of such a
server by hand, test it until it could be trusted, measure it, and write
down what the measurements said, including where they proved an earlier
claim wrong.

What it does:

- Speaks a small binary protocol with length-prefixed frames: PING,
  ECHO, COMPUTE (a hash loop that stands in for CPU work) and STATS
  (the server's own counters).
- Serves thousands of connections from one thread, 10,000 in the
  benchmarks.
- Bounds everything it holds: input and output per connection, requests
  waiting for a worker, connections, and the time a connection may spend
  idle, half-way through a frame, or not reading its answers.
- Says "server busy" when it is, and keeps the connection.
- Shuts down in order: requests already under way are answered, within
  a deadline.

## Architecture

```text
                              clients
                                 |   TCP, frames with a length in front
                                 v
  +----------------------------------------------------------------+
  |  Event loop thread                                             |
  |                                                                |
  |    Poller       epoll or poll(): which sockets are ready       |
  |    TimerQueue   the deadline of every connection               |
  |    Waker        how a worker ends the loop's sleep             |
  |                                                                |
  |    accepts, reads, decodes, answers PING and STATS itself,     |
  |    sends every response                                        |
  |                                                                |
  |    owns the listening socket and every Connection, with its    |
  |    socket and its input and output buffers                     |
  +----------+------------------------------------------^----------+
             |                                          |
             |  task queue                              |  completion queue
             |  bounded: full means "server busy"       |  taken out whole,
             v                                          |  once per turn
  +----------------------------------------------------------------+
  |  Worker threads                                                |
  |                                                                |
  |    run ECHO and COMPUTE. A worker gets a copy of the request   |
  |    and the id of its connection, never the socket, and gives   |
  |    back an encoded response.                                   |
  +----------------------------------------------------------------+
```

One request, from arrival to answer:

1. The poller reports the socket readable. The loop reads what is there
   into the connection's input buffer.
2. The loop decodes one frame. A PING or STATS is answered on the spot.
   Anything else is queued for the workers, and the connection decodes
   nothing more until that request is answered, which is what keeps
   responses in order.
3. Before the loop waits again, it wakes as many sleeping workers as
   there are requests waiting.
4. A worker runs the request and puts the encoded response on the
   completion queue. If the loop is asleep, it wakes it.
5. The loop takes all finished work at once, finds each connection again
   by descriptor and id, and sends the response straight to the socket.
   Only if the socket does not take all of it is the rest kept, and the
   poller asked to say when there is room.

The two threads' worlds meet in exactly two places, the task queue and
the completion queue, each behind its own mutex.

The rest is in the documents:

| | |
| --- | --- |
| [docs/architecture.md](docs/architecture.md) | Modules, ownership, connection states, timeouts, shutdown, every limit |
| [docs/protocol.md](docs/protocol.md) | The wire format, with worked examples in hex |
| [docs/performance.md](docs/performance.md) | Where the time goes, what was changed because of it, what each change did |
| [docs/reliability.md](docs/reliability.md) | Fuzzing, stress and fault tests, the soak test |
| [benchmarks/README.md](benchmarks/README.md) | How the server is measured and how to get any number again |
| [docs/PROGRESS.md](docs/PROGRESS.md) | Step by step: what was done, what was verified and where, what was not, and the open issues |

## Requirements

- CMake 3.20 or newer
- A C++17 compiler (Apple clang, clang, or g++)
- Python 3 for the integration tests
- Network access on the first configure, to fetch GoogleTest

## Build and test

```bash
cmake -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The default build type is `RelWithDebInfo`. Pick another one with
`-DCMAKE_BUILD_TYPE=Debug` or `-DCMAKE_BUILD_TYPE=Release`.

`ctest` runs, in about two minutes on the Mac and three on Linux, where
the integration and stress suites run once for each poller:

| | |
| --- | --- |
| Unit tests | GoogleTest, one program per module |
| `integration_poll`, `integration_epoll` | The real server from the outside, once per poller the platform has |
| `stress_poll`, `stress_epoll` | The server under abuse: bursts, churn, clients that never read or never finish, a full queue, no descriptors, restarts. Each test ends by checking that nothing was left behind and that the server stops cleanly |
| `loadgen` | The load generator against the server |
| `soak_smoke` | Half a minute of the soak test |

Not part of `ctest`: the fuzz targets and the half-hour soak test. Both
are in [docs/reliability.md](docs/reliability.md), with what they found.

## Run

```bash
./build/server --port 8080 --workers 8
```

| Option | Default | Meaning |
| --- | --- | --- |
| `--poller poll\|epoll` | `epoll` on Linux, `poll` elsewhere | How the event loop waits for sockets. epoll exists only on Linux. |
| `--port N` | 8080 | TCP port on 127.0.0.1. |
| `--workers N` | 8 | Worker threads that process requests. |
| `--max-connections N` | 10000 | Connections served at once. Further ones wait in the backlog. |
| `--backlog N` | `SOMAXCONN` | Listen backlog. The kernel may cap it. |
| `--max-frame N` | 1048576 | Largest frame length a client may send, in bytes. |
| `--output-high-water N` | 4194304 | Unsent bytes per connection above which the server stops reading from it. |
| `--idle-timeout-ms N` | 60000 | Closes a connection with nothing buffered and nothing in flight. |
| `--partial-frame-timeout-ms N` | 10000 | Closes a connection whose frame started arriving and did not finish. |
| `--write-stall-timeout-ms N` | 10000 | Closes a connection that takes none of its pending output. |
| `--linger-timeout-ms N` | 2000 | After a framing error, how long to wait for the client to close. |
| `--shutdown-timeout-ms N` | 5000 | After SIGINT or SIGTERM, how long connections get to receive what they are owed. |
| `--queue-capacity N` | 1024 | Requests that may wait for a worker. Beyond that a request is answered with "server busy". |
| `--stats-interval-ms N` | 0 | If not zero, logs one line of counters this often. |

Running the server with an unknown option prints this list with the
defaults.

Ctrl+C or SIGTERM starts an orderly shutdown: no new connections, no new
requests, and every request already with a worker still gets its
response, within `--shutdown-timeout-ms`. A second signal ends the
process at once.

Two limits protect the accept path, and they behave differently:

- **Connection limit reached:** the server stops accepting. New clients
  connect, wait in the kernel backlog, and are served when a slot frees.
- **File descriptors exhausted:** the server accepts each new connection
  only to close it at once, so the client is turned away instead of
  hanging. This happens first when `--max-connections` is higher than
  `ulimit -n` allows.

## Per-connection limits

Each connection is served one request at a time, and responses leave in
the order the requests arrived. What a connection can hold in memory is
bounded:

- **Input:** at most one frame of the largest size (`--max-frame` plus
  4 bytes). Further requests wait in the kernel's socket buffer.
- **In flight:** one request with a worker.
- **Output:** once unsent responses reach `--output-high-water`, the
  server stops reading and decoding for that connection. It resumes at
  half that amount. A client that never reads its responses is slowed
  down by TCP flow control and cannot make the server buffer without end.

## Build options

| Option | Default | Effect |
| --- | --- | --- |
| `WARNINGS_AS_ERRORS` | `OFF` | Adds `-Werror`. CI turns it on. |
| `ENABLE_ASAN` | `OFF` | AddressSanitizer and UndefinedBehaviorSanitizer. |
| `ENABLE_TSAN` | `OFF` | ThreadSanitizer. Cannot be combined with `ENABLE_ASAN`. |
| `ENABLE_FUZZING` | `OFF` | Builds the libFuzzer targets `fuzz_decode` and `fuzz_stream`, with ASan and UBSan. Needs clang. |

Sanitizer builds go in their own build directories:

```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON
```

If a sanitizer binary crashes or hangs before `main` on macOS, the
sanitizer runtime bundled with that Xcode does not match the OS. Use
another clang by adding, for example,
`-DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++`.

## Linux from a Mac

The epoll backend exists only on Linux. The `Dockerfile` gives a Linux
environment that builds the project and runs every test, including the
integration suite once with `poll` and once with `epoll`:

```bash
docker build -t tcpserver-dev .
```

```bash
docker run --rm -v "$PWD":/src:ro tcpserver-dev
```

The source is mounted read-only and the build stays inside the
container. A sanitizer build takes its options from `CMAKE_OPTIONS`:

```bash
docker run --rm -v "$PWD":/src:ro -e CMAKE_OPTIONS="-DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON" tcpserver-dev
```

## Benchmarks

The method, the tools and how to run them are in
[benchmarks/README.md](benchmarks/README.md). What was measured, with
the evidence, is in [benchmarks/RESULTS.md](benchmarks/RESULTS.md). The
raw data of every run is under `benchmarks/results/`, and each report
there starts with the command that produced it.

```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build-release --parallel
```

```bash
python3 benchmarks/run.py --build build-release --suite quick
```

```bash
python3 benchmarks/analyze.py benchmarks/results/<the directory it printed>
```

## Results

All of it was measured on one Apple M1 laptop, client and server
together, over loopback, on 15 November 2026. "Linux" is Ubuntu 24.04
with g++ 13.3 in Docker's virtual machine on that laptop, the server
pinned to four CPUs and the load generator to four others. "Mac" is
macOS 26.6.2 with Apple clang 17, nothing pinned. Release builds, median
of five runs of five seconds, 100 connections and 4 workers unless it
says otherwise. These numbers compare one version of the server with
another on this machine. They are not what a server machine would give.

**Before and after profiling** (commits `4421641` and `dbfc0ad`,
[docs/performance.md](docs/performance.md)):

| | Linux, epoll | Mac, `poll()` |
| --- | ---: | ---: |
| Echo of 64 bytes, requests/s | 109,354 → 325,481 | 112,888 → 153,185 |
| The same with 1 worker | 223,437 → 345,928 | 135,232 → 175,305 |
| Echo of 64 KiB, requests/s | 14,045 → 20,468 | 35,167 → 38,704 |
| Heavy compute, requests/s | 278 → 289 | 291 → 286 |
| Echo at a fixed 30,000 requests/s, median latency | 61 → 55 µs | 196 → 164 µs |
| System calls per echo request | 6.70 → 2.68 | not counted |

The profile showed that over 80% of the event loop's time was in the
kernel, that each response cost two `epoll_ctl` calls it did not need
and each request a `recv` that returned nothing, and that with idle
workers every request woke one. Four changes were kept. A fifth removed
a copy, gained nothing, and was taken out again.

**`poll()` against epoll** (commit `dafec02`, before those changes,
[benchmarks/RESULTS.md](benchmarks/RESULTS.md)):

| Echo of 64 bytes, 100 busy connections | Linux, epoll | Linux, `poll()` | Mac, `poll()` |
| --- | ---: | ---: | ---: |
| with no idle connections beside them | 101,900 | 117,207 | 117,223 |
| with 5,000 idle connections | 99,747 | 39,424 | 6,085 |

`poll()` is handed every descriptor on every call. Idle connections
cost it as much as busy ones, and cost epoll nothing.

**Workers**: heavy compute scales with the cores it is given, 76, 150
and 289 requests/s with 1, 2 and 4 workers on Linux (commit `dafec02`).
Echo does not need the workers and used to be slowed down by them: four
workers did half of what one did (`dafec02`). They now do 94%
(`dbfc0ad`).

**Half an hour under mixed load** (commit `cedb89e`,
[docs/reliability.md](docs/reliability.md)): 139 million requests on the
Mac and 118 million on Linux. The idle server's memory stayed between
2.7 and 4.5 MiB and between 5.0 and 5.2 MiB, and it held 7 descriptors
at the end of every one of 30 cycles, as at the start.

**Fuzzing**: 194 million inputs to the frame decoder and 1.5 million
byte streams through a connection, cut up at random. No failure.

## Design decisions and trade-offs

Each of these is argued at more length in
[docs/architecture.md](docs/architecture.md) or
[docs/PROGRESS.md](docs/PROGRESS.md).

- **One thread owns every socket.** No connection is ever touched by two
  threads, so no connection needs a lock. The price is that one thread's
  capacity is the server's capacity for small requests, and after the
  profiling that is where the limit now is.
- **Workers get a copy of the request, never the connection.** A
  connection can be closed while its request is being worked on. Ids are
  never reused, and a result for a connection that is gone is dropped.
  The price is a copy of every payload.
- **One request per connection at a time.** Responses leave in the order
  requests arrived, with no bookkeeping. The price is that requests
  pipelined on one connection are not run in parallel.
- **A full queue answers "server busy".** The alternative, to stop
  reading from the client until there is room, hides an overloaded
  server from its clients and needs a list of who is waiting. The price
  is an error frame per request for a client that does not back off.
- **Every buffer has a limit, and reaching it has a defined effect.** A
  connection holds at most one frame of input and a bounded amount of
  unsent output. Beyond that the server stops reading from it, and TCP
  flow control slows the client down.
- **Level-triggered polling.** The server may take part of what a
  socket has and be told again. Edge-triggered polling would force
  reading until the socket is empty, which lets one busy client starve
  the others. Measured afterwards: there is little left for it to save.
- **Closing after a bad frame is done in two steps.** The server sends
  its error and its FIN, then keeps reading and dropping until the
  client closes. Closing at once, with input unread, makes the kernel
  send a reset, which can destroy the error the client was just sent.
- **Shutdown finishes what was started.** Requests with a worker are
  answered, within a deadline. Requests received and not yet started
  are dropped, and the client is not told which.
- **Deadlines live in a heap with lazy deletion.** A connection's
  deadline changes far more often than it runs out, so a change costs
  nothing and stale entries are thrown away when they come up.
- **A change to the request path is kept only if it can be measured.**
  One that could not be was reverted, and one that at first could not
  be was reverted and then put back when a second measurement showed
  why the first had missed it.

## Known limitations

- **It listens on 127.0.0.1 only.**
- **No encryption and no authentication.**
- **One event loop thread.** For small requests it is the limit: it is
  on a CPU 93% of the time or more, 70% of that in `send`. Several event
  loops would be the next step and were not built.
- **Every number here is from a laptop,** with the client on the same
  machine, and Linux in a virtual machine. Nothing was measured on
  server hardware or over a network.
- **A request with a worker has no time limit.** The COMPUTE limit is on
  iterations, not on work, so one connection can occupy a worker for a
  long time.
- **With the defaults, descriptors run out before connections do.**
  `--max-connections` is 10,000 and `ulimit -n` is often 256 or 1,024.
  The server turns newcomers away correctly, and does not warn at start.
- **A connection keeps the memory of its largest frame** until it
  closes.
- **`TCP_NODELAY` is not set.** It made no difference over loopback and
  was not tried over a network.
- **An exception that escapes the event loop ends the process.**
- **On this Mac, sanitizers and fuzzing need Homebrew clang,** and the
  fuzz targets a linker Apple has deprecated.

The full list, with numbers, is at the end of
[docs/PROGRESS.md](docs/PROGRESS.md).

## Layout

```text
include/       TcpServer.h, EventLoop.h, Poller.h, PollPoller.h,
               EpollPoller.h, Waker.h, TimerQueue.h, Connection.h,
               RequestHandler.h, Buffer.h, FileDescriptor.h,
               ThreadPool.h, Protocol.h, Config.h
src/           main.cpp and one .cpp per header
tests/         test_buffer.cpp, test_config.cpp, test_connection.cpp,
               test_file_descriptor.cpp, test_histogram.cpp,
               test_poller.cpp, test_protocol.cpp,
               test_request_handler.cpp, test_threadpool.cpp,
               test_timer_queue.cpp, test_waker.cpp (GoogleTest)
               test_server.py (black-box, starts the real server)
               test_stress.py (stress and fault tests)
               test_loadgen.py (the load generator against the server)
               protocol.py (Python client for the protocol)
               fuzz/ (two libFuzzer targets and their corpus)
benchmarks/    loadgen/ (the load generator, C++), run.py (runs a
               suite), analyze.py (report and plots), profile.py
               (profiles the server with perf), flamegraph.py (draws
               a flame graph), soak.py (the soak test), benchmark.py
               (smoke test), README.md (method), RESULTS.md (findings
               of step 9), results/ (raw data of every run),
               profiles/ (every profile)
docs/          architecture.md, protocol.md, performance.md,
               reliability.md, PROGRESS.md
Dockerfile     Linux environment for building and testing
Dockerfile.profile  the same with perf, for profiling
LICENSE        MIT
```

What each module is responsible for is listed at the top of
[docs/architecture.md](docs/architecture.md).

## Licence

[MIT](LICENSE). GoogleTest, which the build fetches for the unit tests,
has its own licence and is not part of this repository.
