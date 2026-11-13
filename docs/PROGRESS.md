# Progress

Updated at the end of each step. "Verified" means it was run, on the
machine named next to it. Results from different machines are not
comparable.

## Steps

- [x] 0. Verify on macOS and set a baseline
- [x] 1. Repository and build infrastructure
- [x] 2. Accept-path robustness and connection limits
- [x] 3. Binary protocol and per-connection buffering
- [ ] 4. Connection lifecycle, half-close, and timeouts
- [ ] 5. Graceful shutdown policy
- [ ] 6. Refactor into modules
- [ ] 7. Linux epoll backend
- [ ] 8. Thread pool engineering
- [ ] 9. Rebuild the benchmark suite
- [ ] 10. Profiling, then optimization
- [ ] 11. Reliability and fault injection
- [ ] 12. Polish

## Environments

- **Mac:** Apple M1, 8 cores, macOS 26.6.2 (25G83), CMake 4.4.5,
  Python 3.14.6.
  - Apple clang 17.0.0 (clang-1700.0.13.3, Xcode 16.3) for normal builds.
  - Homebrew clang 23.1.5 for sanitizer builds (see open issue 2).
- **Linux VM:** Ubuntu 22.04, g++ 11.4, 4 cores. Used before this
  repository existed.
- **Linux container:** Ubuntu 24.04 in Docker on the Mac (aarch64),
  g++ 13.3.0, CMake 3.28.3, Python 3.12.3. Used for Linux-only checks.
- **CI, `ubuntu-latest`:** GitHub-hosted Ubuntu 26.04, g++ 15.2.0.
- **CI, `macos-latest`:** GitHub-hosted macOS 26 on arm64, Apple clang
  21.0.0.

## Step 0: macOS baseline (2026-11-12)

Verified on the Mac, code at tag `v0-baseline`:

- `clang++ -std=c++17 -O2 -Wall -Wextra -pthread main.cpp -o server`
  builds with no warnings.
- `python3 tests/test_server.py <server>`, 35 runs: seven of the eight
  tests passed every time. `test_half_close_still_gets_response` failed
  in 11 of 35 runs, and in 30 of 50 runs on its own (open issue 1).
- TSan and ASan+UBSan builds with Homebrew clang 23.1.5, five suite runs
  each: no sanitizer reports. Also clean after 10,000 echo requests from
  50 concurrent clients followed by SIGINT, with leak detection on.

Verified earlier on the Linux VM, before this repository existed: all
eight tests pass, including under TSan and ASan+UBSan.

## Step 1: repository and build (2026-11-12)

Verified on the Mac, five `ctest` runs per sanitizer configuration and
three for the others, all with `-DWARNINGS_AS_ERRORS=ON` and no warnings:

| Configuration | Compiler | Result |
| --- | --- | --- |
| RelWithDebInfo (default) | Apple clang 17 | 6 of 6 tests pass |
| Release | Apple clang 17 | 6 of 6 tests pass |
| Debug | Apple clang 17 | 6 of 6 tests pass |
| Debug, `ENABLE_ASAN` | Homebrew clang 23.1.5 | 6 of 6 tests pass |
| Debug, `ENABLE_TSAN` | Homebrew clang 23.1.5 | 6 of 6 tests pass |

The six tests are five GoogleTest cases for `ThreadPool` and the Python
integration suite. On macOS the integration suite skips the half-close
test (open issue 1), so it runs seven of its eight tests there.

Configuring with both `ENABLE_ASAN` and `ENABLE_TSAN` stops with an error.

A sanitizer report fails `ctest`: with a data race added on purpose in a
throwaway copy, the TSan build's integration test failed on the report
text. This matters because the tests stop the server with SIGKILL, so
the sanitizer's exit code is never seen.

Verified in CI, GitHub Actions run 43297991514 on commit `1bf2205`: all
six jobs passed, each with 6 of 6 tests, warnings as errors, and no
compiler warnings.

| Runner | Debug ASan+UBSan | Debug TSan | Release |
| --- | --- | --- | --- |
| `ubuntu-latest` (g++ 15.2.0) | pass | pass | pass |
| `macos-latest` (Apple clang 21.0.0) | pass | pass | pass |

This is the first build of the repository on Linux, and the first time
the sanitizers ran with Apple clang. The half-close test runs on the
Linux jobs and is skipped on the macOS jobs.

Not verified:

- Sanitizer builds with Apple clang on the Mac itself (open issue 2).
  They work on the CI runner, which has a newer Xcode.
- Repeated CI runs. One run per job is not evidence against flakiness.

## Step 2: accept path and connection limits (2026-11-12)

What changed: options are named (`--port`, `--workers`,
`--max-connections`, `--backlog`) and parsed into a `Config` struct. At
the connection limit the listening socket is left out of `poll()`. When
descriptors run out, a spare descriptor is given up to accept and close
the pending connection. Any other `accept()` failure that a retry cannot
fix makes the listening socket sit out one `poll()` round, for 100 ms.

Verified on the Mac, three `ctest` runs each, warnings as errors, no
warnings:

| Configuration | Compiler | Result |
| --- | --- | --- |
| Release | Apple clang 17 | 13 of 13 tests pass |
| Debug, `ENABLE_ASAN` | Homebrew clang 23.1.5 | 13 of 13 tests pass |
| Debug, `ENABLE_TSAN` | Homebrew clang 23.1.5 | 13 of 13 tests pass |

The 13 are seven `Config` cases, five `ThreadPool` cases, and the
integration suite, which now has 14 tests (13 run on macOS, the
half-close test is skipped).

Verified in the Linux container, three `ctest` runs each of Release,
Debug with `ENABLE_ASAN`, and Debug with `ENABLE_TSAN`: 13 of 13 pass,
no warnings. All 14 integration tests run there, including the
half-close test.

Verified in CI, GitHub Actions run 43306747759 on commit `23a6a6f`: all
six jobs pass with 13 of 13 tests and no warnings. Two earlier runs each
had one failing Linux job, and both causes were outside the server:

- **Ubuntu ASan+UBSan, commit `964e14f`.** UBSan's vptr check opens a
  pipe every time it runs, to probe the object's memory. With no
  descriptors free the probe fails, and the check reports an invalid
  vptr or aborts. The check is now off on Linux (open issue 8).
- **Ubuntu TSan, commit `a49c925`.** A race in the exhaustion test. The
  readiness probe in `start_server()` could still hold a descriptor
  while the 60 test connections arrived, and release it afterwards, so
  a connection the test expected to be refused was served. Reproduced
  in the container on one CPU: 2 failures in 60 runs, each with 16
  connections served instead of 17. The test now connects until one
  more is refused: 0 failures in 80 runs under the same conditions.

The new tests fail without the fixes. Checked in the Linux container
with the server limited to 24 descriptors and 60 clients connecting:

| Server code | Result |
| --- | --- |
| Before the fix (commit `e1b9882`) | Exhaustion test times out. The server used 2.00 s of CPU in 2 s and wrote about 767,000 log lines. |
| Limit checked only in the accept loop, listener still polled | `test_no_cpu_spin_at_limit` fails: 2.0 s of CPU in 2 s. |
| Current | Both pass. One log line per second. |

On macOS the code before the fix already passes the exhaustion test.
There a failed `accept()` with `EMFILE` drops the pending connection: 43
refused clients produced exactly 43 failures and no spin. The spin is a
Linux behaviour.

Not verified:

- The 100 ms accept backoff. No test makes `accept()` fail in a way the
  spare descriptor cannot fix. `ENFILE` (the system-wide table is full)
  cannot be produced from a test.
- That the `ECONNABORTED` branch runs. The reset-burst test may produce
  it, but nothing confirms that it did.
- The effect of `--backlog`. It is parsed and passed to `listen()`, and
  no test observes the kernel queue.
- The default limit of 10,000 connections at that scale.

## Step 3: binary protocol and buffering (2026-11-13)

What changed: the server speaks the framed protocol in
[protocol.md](protocol.md) instead of echoing raw bytes. New units are
`Buffer` (byte queue with a read offset) and `Protocol` (encoder,
decoder, request validation, the COMPUTE hash). Each connection has an
input buffer, an output buffer with a high and a low watermark, and at
most one request with a worker.

Decisions that differ from, or add to, the project brief:

- **No queue of parsed requests.** The brief asked for a FIFO of up to
  64 parsed requests per connection. With 1 MiB frames that queue could
  hold 64 MiB per connection. Waiting requests now stay as raw bytes in
  the input buffer and are decoded one at a time. Approved by the
  project owner on 2026-11-13.
- **The input buffer holds one frame of the largest size**, not more.
  Anything beyond that waits in the kernel socket buffer.
- **`--max-frame` limits the length field**, so the largest payload is
  the limit minus 8.
- **PING ignores its payload** and answers with an empty one.
- **Error code 7, bad request**, was added for a COMPUTE payload that
  is too short or asks for more than 100,000 iterations.
- **`main.cpp` and `ThreadPool.h` were clang-formatted** in their own
  commit before the rewrite. The compiled objects were byte-identical
  before and after.

Verified on the Mac, three `ctest` runs each, warnings as errors, no
warnings:

| Configuration | Compiler | Result |
| --- | --- | --- |
| Release | Apple clang 17 | 44 of 44 tests pass |
| Debug, `ENABLE_ASAN` | Homebrew clang 23.1.5 | 44 of 44 tests pass |
| Debug, `ENABLE_TSAN` | Homebrew clang 23.1.5 | 44 of 44 tests pass |

The 44 are 43 GoogleTest cases (Buffer 7, Config 7, Protocol 24,
ThreadPool 5) and the integration suite, which now has 32 tests (30 run
on macOS, the two half-close tests are skipped).

Verified in the Linux container, three `ctest` runs each of Release,
Debug with `ENABLE_ASAN`, and Debug with `ENABLE_TSAN`: 44 of 44, no
warnings. All 32 integration tests run there.

Repeated runs of the integration suite, looking for timing problems: 15
runs under TSan in the Linux container limited to one CPU, and 10 runs
of the Release build on the Mac. No failures.

The new tests fail when the behaviour they check is removed (Mac,
Release build):

| Change to the server | Result |
| --- | --- |
| Started with a high watermark it can never reach | The slow-reader test fails. The server took all 64 MiB offered and grew by 67 MiB. With the watermark at 1 MiB it took 2.2 MiB and grew by 3.5 MiB. |
| Decoding continues while a request is with a worker | Both ordering tests fail, because PING answers overtake earlier requests. |

Verified in CI, GitHub Actions run 43404943388 on commit `9aafdcf`: all
six jobs pass on the first run, each with 44 of 44 tests and no
warnings.

Not verified:

- Error codes 5 (server busy) and 6 (internal error) are defined and
  never sent.
- The exact watermark boundaries. The slow-reader test shows that
  throttling starts and that the connection recovers, not at which byte.
- Any performance effect of the protocol and the extra copies. Nothing
  was measured in this step.
- Behaviour with thousands of connections at their memory limits.

## Open issues

1. **Half-close is broken on macOS.** `poll()` on macOS reports
   `POLLIN|POLLHUP` as soon as the peer calls `shutdown(SHUT_WR)`, even
   with unread data. The server closes on `POLLHUP` before reading
   (`src/main.cpp`, the `POLLERR | POLLHUP | POLLNVAL` check). When the
   FIN arrives before `recv()`, the request is dropped and the client
   gets `ECONNRESET`. `test_half_close_still_gets_response` is skipped on
   macOS until this is fixed in step 4. Remove the skip then.
2. **Apple clang sanitizers do not start on the Mac.** With Xcode 16.3
   and Command Line Tools 26.2 on macOS 26.6.2, a TSan binary segfaults
   and an ASan binary hangs before `main`, even for a hello-world.
   Sanitizer builds on this machine need
   `-DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++`.
3. **No test triggers a worker exception.** The `Completion::failed`
   path in the server has never run under any test.
4. *Resolved in step 3.* All sources are now clang-formatted.
5. **`.clang-format` is not enforced in CI.**
6. **The default connection limit is above common descriptor limits.**
   `--max-connections` defaults to 10,000, while `ulimit -n` is often
   256 on macOS and 1,024 on Linux. With defaults, descriptors run out
   first and new clients are closed instead of queued. The server does
   not warn about this at startup and does not raise its own limit.
7. **Clients waiting at the connection limit get no signal.** They sit
   in the kernel backlog until a slot frees. Once the backlog is full
   the kernel ignores further connection attempts.
8. **UBSan's vptr check is off on Linux.** It needs a free descriptor
   on every check, which the descriptor exhaustion test takes away. The
   other undefined-behaviour checks and ASan are unaffected, and macOS
   keeps the check.
9. **Closing after a framing error can lose the ERROR frame.** The
   server stops reading, sends the ERROR, and closes. If the client is
   still sending, unread bytes are left in the socket at `close()`, and
   the kernel then sends RST instead of FIN. A reset can discard data
   the client has not read yet. The tests avoid this by sending nothing
   after the bad length. Step 4 should send FIN with
   `shutdown(SHUT_WR)` and keep reading until the client closes.
10. **The COMPUTE limit bounds iterations, not work.** The cost is
    iterations times data length. The largest request, 100,000 passes
    over about 1 MiB, keeps one worker busy for a long time, cannot be
    cancelled, and makes shutdown wait for it.
11. **A worker exception closes the connection.** Step 8 turns it into
    ERROR 6.
12. **`Buffer` keeps its capacity.** After one large frame a connection
    holds that much memory until it closes, even when idle. It is
    bounded by the limits, but not returned.
13. **A client that never reads is throttled, not dropped.** It keeps
    its connection and its buffered output forever. Step 4 adds the
    write-stall timeout.
