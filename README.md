# TCP Server

A concurrent TCP server in C++17 on POSIX. A single-threaded `poll()` event
loop owns all sockets and hands each request to a worker thread pool.

The server currently echoes whatever it receives. Framing, a binary
protocol, timeouts, graceful shutdown, and an epoll backend are planned.
See [docs/PROGRESS.md](docs/PROGRESS.md) for what is done, what has been
verified, and the open issues.

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
| `--port N` | 8080 | TCP port on 127.0.0.1. |
| `--workers N` | 8 | Worker threads that process requests. |
| `--max-connections N` | 10000 | Connections served at once. Further ones wait in the backlog. |
| `--backlog N` | `SOMAXCONN` | Listen backlog. The kernel may cap it. |

The server stops on Ctrl+C or SIGTERM.

Two limits protect the accept path, and they behave differently:

- **Connection limit reached:** the server stops accepting. New clients
  connect, wait in the kernel backlog, and are served when a slot frees.
- **File descriptors exhausted:** the server accepts each new connection
  only to close it at once, so the client is turned away instead of
  hanging. This happens first when `--max-connections` is higher than
  `ulimit -n` allows.

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

## Layout

```text
include/   Config.h, ThreadPool.h
src/       main.cpp, Config.cpp
tests/     test_config.cpp, test_threadpool.cpp (GoogleTest)
           test_server.py (black-box, starts the real server)
docs/      PROGRESS.md
```
