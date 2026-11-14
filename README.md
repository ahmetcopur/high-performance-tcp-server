# TCP Server

A concurrent TCP server in C++17 on POSIX. A single-threaded `poll()` event
loop owns all sockets and hands each request to a worker thread pool.

Clients talk to it in a small binary protocol with length-prefixed
frames: PING, ECHO, COMPUTE (a hash loop that stands in for CPU work),
and STATS (the server's counters). The format is in [docs/protocol.md](docs/protocol.md).

How connections move through their states, what times out and when, and
every limit the server enforces are in
[docs/architecture.md](docs/architecture.md).

See [docs/PROGRESS.md](docs/PROGRESS.md) for what is done, what has
been verified, and the open issues.

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

## Layout

```text
include/       TcpServer.h, EventLoop.h, Poller.h, PollPoller.h,
               EpollPoller.h, Waker.h, TimerQueue.h, Connection.h,
               RequestHandler.h, Buffer.h, FileDescriptor.h,
               ThreadPool.h, Protocol.h, Config.h
src/           main.cpp and one .cpp per header (Poller is an
               interface and has none)
tests/         test_buffer.cpp, test_config.cpp, test_connection.cpp,
               test_file_descriptor.cpp, test_poller.cpp,
               test_protocol.cpp, test_request_handler.cpp,
               test_threadpool.cpp, test_timer_queue.cpp,
               test_waker.cpp (GoogleTest)
               test_server.py (black-box, starts the real server)
               protocol.py (Python client for the protocol)
docs/          architecture.md, protocol.md, PROGRESS.md
Dockerfile     Linux environment for building and testing
benchmark.py   quick load script, not a rigorous benchmark
```

What each module is responsible for is listed at the top of
[docs/architecture.md](docs/architecture.md).
