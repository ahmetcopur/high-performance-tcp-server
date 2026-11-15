# Progress

Updated at the end of each step. "Verified" means it was run, on the
machine named next to it. Results from different machines are not
comparable.

## Steps

- [x] 0. Verify on macOS and set a baseline
- [x] 1. Repository and build infrastructure
- [x] 2. Accept-path robustness and connection limits
- [x] 3. Binary protocol and per-connection buffering
- [x] 4. Connection lifecycle, half-close, and timeouts
- [x] 5. Graceful shutdown policy
- [x] 6. Refactor into modules
- [x] 7. Linux epoll backend
- [x] 8. Thread pool engineering
- [x] 9. Rebuild the benchmark suite
- [x] 10. Profiling, then optimization
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
  g++ 13.3.0, CMake 3.28.3, Python 3.12.3. Used for Linux-only checks
  and, since step 9, for the Linux benchmarks. It runs in a virtual
  machine with 8 CPUs, kernel 7.0.14-linuxkit.
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

## Step 4: connection lifecycle and timeouts (2026-11-13)

What changed: a connection is now in one of three states (Open,
ReadClosed, Closing) and one of five phases (idle, partial frame, busy,
writing, lingering), each phase but busy with a time limit. `POLLHUP`
is no longer treated as a dead connection. After a framing error the
server sends FIN and keeps reading until the client closes. All of it
is described in [architecture.md](architecture.md).

Decisions that add to the project brief:

- **A fourth timeout, `--linger-timeout-ms`** (default 2 s), bounds the
  wait for the client to close after a framing error.
- **`shutdown(SHUT_WR)` is used in Closing, not in ReadClosed.** In
  ReadClosed everything up to the peer's FIN has been read, so a plain
  `close()` already sends FIN. In Closing the peer may still be sending,
  which is where `close()` would turn into a reset.
- **The idle clock does not run while a request is with a worker.** A
  slow COMPUTE is the server's delay, not the client's.
- **The partial-frame clock is not restarted by more bytes of the same
  frame**, only by completing it.
- **Options are parsed from a table**, and the usage message is
  generated from it.

Verified on the Mac, three `ctest` runs each, warnings as errors, no
warnings:

| Configuration | Compiler | Result |
| --- | --- | --- |
| Release | Apple clang 17 | 45 of 45 tests pass |
| Debug, `ENABLE_ASAN` | Homebrew clang 23.1.5 | 45 of 45 tests pass |
| Debug, `ENABLE_TSAN` | Homebrew clang 23.1.5 | 45 of 45 tests pass |

The 45 are 44 GoogleTest cases and the integration suite, which now has
47 tests. None is skipped on macOS any more.

Verified in the Linux container, three `ctest` runs each of Release,
Debug with `ENABLE_ASAN`, and Debug with `ENABLE_TSAN`: 45 of 45, no
warnings. Ten more runs of the integration suite under TSan on one CPU:
no failures.

The new tests against the server from before this change (commit
`0d297dc`, which accepts the new options and ignores them):

| Test | Before | Now |
| --- | --- | --- |
| Four half-close tests, 60 runs, Mac | 60 of 60 runs fail | 0 of 60 |
| Idle timeout (3 tests), Mac | fail | pass |
| Slowloris, Mac | fail | pass |
| Client that never reads is cut off, Mac | fail | pass |
| Descriptor count after 300 connections, Mac | fails, on the half-close connections | pass |
| Client keeps sending after a bad length, 40 runs, Mac | 5 fail with a reset | 0 |
| The same, Linux container | 1 fails with a reset | 0 |

These pass before and after, so they guard against regressions but did
not catch a fault: resets with a request in flight, the linger timeout
(the old server closed at once), and a frame that finishes in time.

One more test was checked the other way round. With the line that
restarts the stall clock on every send removed, "large response read
slowly is not cut off" fails after 0.77 s. With it, the test passes.

Verified in CI, GitHub Actions run 43444250576 on commit `9288268`: all
six jobs pass with 45 of 45 tests and no warnings. Three earlier runs
failed, each because of a test and not the server:

- **All three macOS jobs, commits `78e9bd3` and `cb44243`.** A test
  with a reader taking 32 KiB every 30 ms against a 500 ms write-stall
  limit. On the CI runner the pause took about 70 ms, and at that pace
  the server really cannot send for more than 500 ms at a time (open
  issue 17). Reproduced on the Mac by slowing the reader to 70 ms. The
  test also had no teeth: it passed with the stall clock broken on
  purpose. It was replaced by one large response read at a fixed rate.
- **Ubuntu TSan, commit `41a0951`.** A test abandoned twenty of the
  heaviest COMPUTE requests. They kept every worker busy, and the echo
  after them timed out in the queue. The requests were made lighter.

Not verified:

- The default timeout values. Every timeout test runs with 500 ms.
- The `SO_ERROR` log line on macOS. In the Linux container the tests
  produce it ("Connection reset by peer"). On the Mac they never do,
  because resets show up there as failed reads or writes.
- Which branch each hangup takes. Nothing measures code coverage, so
  "POLLHUP while not reading" may never have run.
- The cost of scanning every connection for its deadline when there are
  thousands of them.
- How large the announcement steps described in open issue 17 are on
  Linux. They were measured on macOS only.

Note for step 7: the deadline scan is linear and runs every iteration.
That is free while `poll()` rebuilds its set every iteration too. The
epoll backend needs a heap or a timer wheel.

## Step 5: graceful shutdown (2026-11-13)

What changed: the first SIGINT or SIGTERM closes the listening socket,
drops requests that have not been started, lets the request each
connection has with a worker finish and sends its response, then closes.
`--shutdown-timeout-ms` (default 5 s) bounds all of it. A second signal
ends the process at once. The policy and the order of destruction are in
[architecture.md](architecture.md).

Decisions that add to the project brief:

- **Requests received but not started are dropped.** "Already queued"
  is taken to mean handed to the worker pool. With one request per
  connection in flight, that is at most one per connection.
- **A draining connection ends through Closing**, with FIN and a wait
  for the client's close, so that a client still sending cannot turn
  the close into a reset that destroys its last response.
- **Abandoned work is cancelled at the end of a shutdown.** COMPUTE
  checks a flag before each pass. Without it, joining the workers held
  the process long after the deadline.
- **The second signal uses the default action.** The handler restores
  it, so the process dies by the signal and its exit status says so.
- **Reaching the deadline still exits with code 0.** One log line
  reports how many connections were cut.

Verified on the Mac, three `ctest` runs each, warnings as errors, no
warnings:

| Configuration | Compiler | Result |
| --- | --- | --- |
| Release | Apple clang 17 | 47 of 47 tests pass |
| Debug, `ENABLE_ASAN` | Homebrew clang 23.1.5 | 47 of 47 tests pass |
| Debug, `ENABLE_TSAN` | Homebrew clang 23.1.5 | 47 of 47 tests pass |

The 47 are 46 GoogleTest cases and the integration suite, which now has
54 tests.

Verified in the Linux container, three `ctest` runs each of Release,
Debug with `ENABLE_ASAN`, and Debug with `ENABLE_TSAN`: 47 of 47, no
warnings.

Verified in CI, GitHub Actions run 43448740809 on commit `97dc100`: all
six jobs pass with 47 of 47 tests and no warnings. The macOS ASan job
needed a second attempt. The first was cancelled after 15 minutes in
the queue because GitHub had no macOS runner free to take it. It never
started, so that says nothing about the code.

The new tests against the server from before this change (commit
`4f1f1d4`), on the Mac: six of the seven shutdown tests fail. The one
that passes, idle connections closed at once, was already true. With
only the cancellation of abandoned work removed from the current
server, the test with a request that runs for minutes times out.

Not verified:

- That every worker has stopped before the process exits, other than
  by the process exiting in time.
- A shutdown with thousands of connections owed a response.
- The default deadline of 5 s. The deadline tests use 1 s.
- An exception escaping the event loop. Nothing provokes one.

## Step 6: refactor into modules (2026-11-14)

What changed: `main.cpp` went from about 800 lines to 64. The server now
consists of `TcpServer`, `EventLoop`, `Poller` with `PollPoller`,
`Connection` and `FileDescriptor`, next to the `Protocol`, `Buffer`,
`Config` and `ThreadPool` that already existed. What each is responsible
for, and the ownership rules, are in [architecture.md](architecture.md).
No behaviour is meant to have changed.

How it was done: each new module was added with its own unit tests
while `main.cpp` still ran its old code (three commits), and then the
server was switched over in one commit. Every commit builds and passes
its tests.

Decisions:

- **`EventLoop` runs the loop and calls a handler.** It has four
  callbacks: shutdown requested, request finished, timers, descriptor
  ready. `TcpServer` implements them. The loop knows nothing about
  connections or the protocol.
- **Timers stay a linear scan**, done by `TcpServer` in the timers
  callback. `EventLoop` only turns the answer into the sleep time. The
  heap or timer wheel is still due in step 7.
- **A connection's interest is updated in that same scan**, once per
  iteration, by calling `Poller::modify()`. The pollfd array itself is
  no longer rebuilt.
- **A connection is now looked at only when it has an event or a
  completion.** The old loop ran the serve, FIN and finished checks on
  every connection in every iteration. The order of work within an
  iteration is otherwise unchanged.
- **Startup failures are exceptions.** The constructors of `EventLoop`
  and `TcpServer` throw `std::system_error`, and `main` prints it and
  returns 1. The messages name the call that failed, for example
  "bind: Address already in use".
- **`EpollPoller.h` from the target structure does not exist yet.** It
  belongs to step 7.

New unit tests: `FileDescriptor` 7, `PollPoller` 10, `Connection` 26.
The `Connection` tests put one on a socket pair and play the client.
They settle three things earlier steps listed as not verified:
throttling starts exactly at the high watermark, a partial frame is
timed from its first byte, and sending output restarts the stall clock
while a full socket does not.

Verified in CI, GitHub Actions run 43531757713 on commit `98a8cd9`: all
six jobs pass on the first run, each with 90 of 90 tests and no
warnings. The 90 are 89 GoogleTest cases and the integration suite of
54 tests. This run is the only verification of the refactored server
under ASan, UBSan and TSan, and the only one on Linux.

Verified on the Mac, Release build, Apple clang 17: `ctest` passed 90 of
90 each time a run completed without the machine sleeping, three times
in all. Runs interrupted by sleep are not counted either way (open
issue 20). The sanitizer builds and the Linux container were not run
locally for this step.

Only `FileDescriptor.cpp` calls `close()`, checked with grep.

Not verified:

- That behaviour is unchanged in every respect. The evidence is that
  the same 54 integration tests pass. They were not changed in this
  step.
- The exact low watermark. The test shows a connection stays throttled
  until its output has drained, not the byte at which it resumes.
- Any performance effect of the refactor. Nothing was measured.

## Step 7: epoll backend (2026-11-14)

What changed: the event loop can wait with epoll on Linux
(`--poller epoll`, the default there) or with `poll()` (`--poller poll`,
the only choice elsewhere). Wakeups use an eventfd on Linux and a pipe
elsewhere. Deadlines are kept in a min-heap with lazy deletion, and
nothing visits every connection per iteration any more. A `Dockerfile`
gives a Linux environment for running all of it from a Mac. The design
is in [architecture.md](architecture.md) under Pollers and Timeouts.

Decisions:

- **`EPOLLRDHUP` is requested only together with input** and reported
  as a hangup. Requested always, it would make a connection that has
  stopped reading look hung up while it still owes responses.
- **The timer queue promises "no later than".** A deadline that moves
  further away pushes nothing. The entry comes up at the old time and
  the connection is armed again. The heap stays at about one entry per
  connection.
- **Interest and deadline are updated per event**, at the end of
  `TcpServer::advance()`, where the previous step updated them for
  every connection on every iteration.
- **`Poller::add()` can fail** and returns false. epoll has a system
  limit on watched descriptors. A connection that cannot be watched is
  dropped at accept.
- **`Waker` is its own small class**, so that the difference between
  eventfd and a pipe is in one file.
- **Edge-triggered mode is not implemented.** The brief makes it
  conditional on a measured benefit, and nothing has been measured yet.
  Why it is harder is written up in architecture.md.
- **The option is `--poller epoll`**, two words like every other
  option, where the brief wrote `--poller=epoll`.

How the two backends are held to the same behaviour:

- The poller unit tests are one suite that runs against `PollPoller`
  and, on Linux, `EpollPoller`: 14 tests each.
- CTest registers the integration suite once per poller the platform
  has: `integration_poll` everywhere and `integration_epoll` on Linux.

New tests: `Waker` 7, `TimerQueue` 15, pollers 5 more shared and 3 for
epoll only, `Config` 5, and one integration test, "idle connections
cost no CPU". Run against an `EpollPoller` changed to always register
`EPOLLOUT`, that test fails with 1.99 s of CPU in 2 s.

Verified in the Linux container (Ubuntu 24.04, g++ 13.3.0), warnings
as errors, no warnings. Every run includes the integration suite on
both pollers:

| Configuration | Result |
| --- | --- |
| Release | 140 of 140 tests pass |
| Debug, `ENABLE_ASAN`, two runs | 140 of 140 |
| Debug, `ENABLE_TSAN`, two runs | 140 of 140 |
| Image built from the `Dockerfile`, default command | 140 of 140 |

The 140 are 138 GoogleTest cases, `integration_poll` and
`integration_epoll`. The integration suite has 55 tests.

With `--poller epoll` a running server holds one epoll instance and one
eventfd. With `--poller poll` on Linux it holds the eventfd only.
Checked by listing `/proc/<pid>/fd`.

Verified on the Mac, two `ctest` runs each, warnings as errors, no
warnings. epoll does not exist there, so this is `poll` with a pipe:

| Configuration | Compiler | Result |
| --- | --- | --- |
| Release | Apple clang 17 | 122 of 122 tests pass |
| Debug, `ENABLE_ASAN` | Homebrew clang 23.1.5 | 122 of 122 |
| Debug, `ENABLE_TSAN` | Homebrew clang 23.1.5 | 122 of 122 |

`--poller epoll` is refused on the Mac with a message.

Step 6 follow-up: the refactored server from step 6 was run under
ASan+UBSan on the Mac after the machine was awake again, 90 of 90
twice. Step 6 had recorded that as not done locally.

Verified in CI, GitHub Actions run 43592879117 on commit `9dbf657`: all
six jobs pass on the first run, no warnings. The three Ubuntu jobs
(g++ 15.2.0) run 140 tests each, with `integration_poll` and
`integration_epoll` both passing. The three macOS jobs run 122.

Not verified:

- That epoll is faster. Nothing was measured. The benchmark suite is
  step 9.
- `Poller::add()` failing in the running server. The unit tests make it
  fail for a closed descriptor and a duplicate, not for the system
  limit.
- The server with thousands of connections on either backend.
- Edge-triggered mode, which does not exist.

## Step 8: thread pool (2026-11-14)

What changed: the worker pool's queue is bounded
(`--queue-capacity`, default 1024) and `trySubmit()` returns false when
it is full. A request that finds it full is answered with ERROR 5,
"server busy", and the connection stays open. A request that fails
inside a worker is answered with ERROR 6, "internal error", where the
connection used to be closed. Tasks are stored as move-only callables
with one allocation each. The pool keeps counters, which the new STATS
request reports and `--stats-interval-ms` logs. Details are in
[architecture.md](architecture.md) under "The worker pool" and in
[protocol.md](protocol.md).

Decisions:

- **Full queue: answer "server busy", do not stop reading.** The brief
  asked for this choice and the alternative to be written down. Both
  are in architecture.md.
- **A refused task is left untouched.** `trySubmit()` checks for room
  before it builds anything, so nothing is moved out of the caller's
  callable.
- **The capacity counts waiting tasks**, not the ones being run.
- **The future-returning `enqueue()` is gone.** Nothing used the
  future.
- **A task that throws is caught by the pool** and counted. The worker
  goes on.
- **STATS is answered on the event loop**, like PING, so it works while
  every worker is busy. It carries ten 64-bit counters, and fields may
  be added at the end.
- **`RequestHandler` is a new small module** with what a worker does
  with a request. It was a function inside `TcpServer.cpp` and could
  not be unit-tested there.
- **A completion with an empty response closes the connection.** It
  replaces the `failed` flag, and only happens if a worker cannot build
  even the error.
- **The completion queue already drained by swapping** since step 3,
  and already woke the loop only on empty to non-empty. Nothing changed
  there.

New tests: `ThreadPool` rewritten, 15 tests, among them refusal at
capacity, a refused task left intact, each producer's order with one
worker, a throwing task, shutdown with tasks queued, and eight
producers into a queue of 16 with four workers, every task run exactly
once. `RequestHandler` 9. Protocol 5 for STATS. Config 1. Integration
8: five for STATS, the full queue, and the log line on and off.

With the queue made so large that it never fills, the full-queue test
fails: the late request waits 1.7 s for the worker where it should be
refused within 0.5 s.

Verified in the Linux container (image from the `Dockerfile`, Ubuntu
24.04, g++ 13.3.0), two `ctest` runs each, warnings as errors, no
warnings, integration suite on both pollers:

| Configuration | Result |
| --- | --- |
| Release | all tests pass |
| Debug, `ENABLE_ASAN` | 165 of 165 |
| Debug, `ENABLE_TSAN` | 165 of 165 |

The Release runs were started before the last two unit tests were
added and ran 163. The 165 are 163 GoogleTest cases, `integration_poll`
and `integration_epoll`. The integration suite has 63 tests.

Verified on the Mac, two `ctest` runs each, warnings as errors, no
warnings:

| Configuration | Compiler | Result |
| --- | --- | --- |
| Release | Apple clang 17 | 147 of 147 tests pass |
| Debug, `ENABLE_ASAN` | Homebrew clang 23.1.5 | 147 of 147 |
| Debug, `ENABLE_TSAN` | Homebrew clang 23.1.5 | 147 of 147 |

The pool's stress test, eight producers and four workers on a queue of
16, is part of every one of these runs, including TSan on both
platforms.

Verified in CI, GitHub Actions run 43618623422 on commit `b474ad0`: all
six jobs pass on the first run, no warnings. The Ubuntu jobs run 165
tests, the macOS jobs 147.

The run after it, on a commit that changed only documentation, failed
in one job (Ubuntu ASan+UBSan). A STATS test read `tasks_completed` as
0 just after an ECHO had been answered. The test was wrong, not the
server: a worker hands its result to the event loop first and is
counted as done a moment later, so the counters that workers keep can
trail a response. Three tests assumed otherwise. They now ask again
until the counters have caught up, and protocol.md says which counters
are exact and which can trail. Repeated under ASan on one CPU in the
Linux container, 15 runs per poller: no failures.

Not verified:

- ERROR 6 from a running server. Nothing a client can send makes a
  worker throw. The conversion is unit-tested in `RequestHandler`, and
  the pool's handling of a throwing task in `ThreadPool`.
- The empty-completion path, for the same reason.
- Whether one allocation per task matters. Nothing was measured.
- The pool under real load. The stress test is a unit test.

## Step 9: benchmark suite (2026-11-15)

What changed: the old `benchmark.py` measured a Python client more than
the server. There is now a load generator in C++
(`benchmarks/loadgen/`), a runner that drives it through named suites
and keeps every run (`benchmarks/run.py`), and a report generator
(`benchmarks/analyze.py`). `benchmark.py` moved to `benchmarks/` and is
a smoke test. The method is in
[benchmarks/README.md](../benchmarks/README.md), the findings in
[benchmarks/RESULTS.md](../benchmarks/RESULTS.md), and the raw data in
`benchmarks/results/`.

The load generator runs N connections over T threads, each thread with
its own poller. It makes every connection before it sends anything,
throws a warm-up away, and counts bytes at the socket. Closed loop sends
the next request when the answer arrives. Open loop (`--rate`) keeps a
schedule and counts latency from when a request was due. It reports how
every request ended (completed, busy, error, timeout, connection lost,
unfinished, never sent), a full latency histogram, and its own CPU use
and how well it kept its schedule.

The runner starts a fresh server for every repetition, five per cell,
and samples the server four times a second: CPU time, memory, on Linux
context switches and open files, and queue depth and worker time from
STATS. Every result set records machine, OS, kernel, compiler, flags,
build type, commit, power source and its own command line.

Decisions:

- **The histogram is written here, not taken from a library.**
  Logarithmic buckets, 128 to each power of two, so a value is kept to
  within 0.8% of itself. About 180 lines, with tests.
- **A percentile needs ten samples above it** or it is left out. The
  99.9th percentile of 500 requests is one request.
- **A percentile is the top of its bucket**, so it never understates.
- **The open-loop client spins for the last millisecond before a
  request is due.** Sleeping in `poll()` is not precise enough. So in an
  open loop the client's CPU use says nothing, and a run is judged by
  how late requests were sent: flagged when over 1% went out more than a
  millisecond late.
- **A closed-loop run is flagged when the client's threads are 85% busy
  or more.**
- **The latency suite uses two client threads, not four.** With four,
  the client on the Mac ran up to 110 ms behind its schedule. Both sets
  of results are kept.
- **The client always waits the best way the platform has.** The
  runner's `--poller` is for the server only. Passing it to both, as the
  first version did, slowed both and measured neither.
- **The load generator closes its connections with a reset.** Closing
  thousands of connections normally leaves each port unusable for a
  while, and the next run could not connect.
- **The plots are SVG written by hand.** matplotlib is not installed,
  and the report generator should run anywhere Python does.
- **The poller factory moved into the library**, so the load generator
  uses the same `Poller` as the server.
- **The runner refuses anything but a Release build** unless told
  otherwise, and checks for a sleeping machine by comparing the wall
  clock with a clock that stops during sleep. A run taken while the
  machine slept is redone, up to three times.
- **Raw results are committed**, 7.3 MB for 15 result sets. The brief
  asks for evidence to be kept.

Results. Every figure is the median of five runs of five seconds after
two of warm-up, Release build of commit `dafec02`, client and server on
the same Apple M1 over loopback, 100 connections and 4 workers unless it
says otherwise. "Linux" is the container, g++ 13.3.0, server pinned to
CPUs 0 to 3 and client to 4 to 7. "Mac" is macOS 26.6.2, Apple clang 17,
nothing pinned, on battery. RESULTS.md has all of them with links to the
reports.

| | Linux, epoll | Mac, `poll()` |
| --- | ---: | ---: |
| Echo 64 B, requests/s | 112,807 | 122,046 |
| Echo 64 KiB, requests/s | 14,774 | 30,491 |
| Echo 64 B with 5,000 idle connections | 99,747 | 6,085 |
| Light compute | 160,595 | 110,779 |
| Heavy compute | 289 | 286 |

The worker-scaling experiment, requests per second:

| | 1 worker | 2 | 4 | 8 |
| --- | ---: | ---: | ---: | ---: |
| Linux, echo | 213,639 | 128,064 | 99,541 | 102,193 |
| Linux, light compute | 103,264 | 213,497 | 161,972 | 138,541 |
| Linux, heavy compute | 76 | 150 | 289 | 289 |
| Mac, echo | 130,807 | 118,626 | 106,829 | 95,912 |
| Mac, light compute | 88,232 | 119,499 | 96,297 | 94,398 |
| Mac, heavy compute | 75 | 147 | 286 | 441 |

What they show:

- **Heavy compute scales with the cores it has.** Linux stops at four
  workers because the server was pinned to four CPUs.
- **For cheap requests the workers are not the limit**, and more of
  them make it slower. The server uses 1.1 to 1.4 cores while its
  workers are 1% to 8% busy. *Corrected in step 10:* this first said
  that the event loop thread is the limit. A profile showed it is on a
  CPU only 71% of the time with four workers.
- **The likely cost is waking a sleeping worker for each request.** On
  Linux the server's threads give up the CPU 23,323 times a second with
  one worker on echo and 88,725 times with eight, while throughput
  halves. With two workers on light compute, both 96% busy, it is 2,166
  times and the throughput is the best measured. This is read from
  counters, not profiled. It is where step 10 starts.
- **Idle connections cost `poll()` and not epoll.** With 5,000 idle
  connections the server on Linux does 99,747 with epoll and 39,424
  with `poll()`. With every connection busy the two are level.
- **A closed loop's latency is its queue.** At 100 connections the
  median is 1.06 ms, which is 100 divided by the throughput. At half the
  load in an open loop it is 49 µs, with a 99th percentile of 124 µs
  (Linux, two client threads).

New tests: `test_histogram.cpp`, 12 GoogleTest cases. `test_loadgen.py`,
12 tests that run the load generator against the real server, registered
with CTest as `loadgen`: byte counts against the frame size, the
histogram holding every sample, percentiles left out, the rate kept in
an open loop, busy answers and timeouts counted apart, connections that
could not be made, and one that shows coordinated omission: a server
asked for four times what it can do gives the same throughput in both
modes, while the open loop's median latency is more than five times the
closed loop's.

Verified on the Mac, warnings as errors, no warnings:

| Configuration | Compiler | Result |
| --- | --- | --- |
| RelWithDebInfo | Apple clang 17 | 160 of 160 tests pass |
| Debug, `ENABLE_ASAN` | Homebrew clang 23.1.5 | 160 of 160 |
| Debug, `ENABLE_TSAN` | Homebrew clang 23.1.5 | 160 of 160 |

The 160 are 158 GoogleTest cases, `integration_poll` and `loadgen`.

Verified in the Linux container (Ubuntu 24.04, g++ 13.3.0), integration
suite on both pollers:

| Configuration | Result |
| --- | --- |
| RelWithDebInfo | 178 of 178 |
| Debug, `ENABLE_ASAN` | 178 of 178 |
| Debug, `ENABLE_TSAN` | 178 of 178 |

The first ASan run on the Mac failed one new test, the one that shows
coordinated omission. Its request was sized for an optimized build.
Under ASan the server completed fewer than twenty in two seconds, too
few for a median, and the test compared medians. The request is now a
twentieth of the size and the test compares means. Measured afterwards,
the open loop's mean is 210, 90 and 44 times the closed loop's in the
RelWithDebInfo, ASan and TSan builds, where the test asks for 5. The
test then passed three times in a row in each of the three builds.

The first CI run, 43698651809 on commit `0a4a13d`, passed in five jobs
and failed in one, macOS TSan, in another new test. The open-loop test
asked for 1000 requests a second, give or take 30, and got 963. Each
client thread spins for the last millisecond before a request is due.
At 500 a second per thread that is half its time, beside a TSan server
on a machine with three cores. The test now asks for 200 a second and
allows a tenth either way, which still catches a rate that is wrong by a
factor. The failure was not reproduced: the old test passed five times
on the Mac under TSan with eight busy loops beside it, and six times in
the Linux container under TSan on two CPUs.

The smoke test, `benchmarks/benchmark.py`, against the Release build on
the Mac: 50 clients, every response correct, byte counts as expected.

The benchmarks themselves: 15 result sets, 410 runs, none discarded for
a sleeping machine and none marked suspect. Every figure in RESULTS.md
was recomputed from the raw run files after it was written. That found
four statements the data did not support, which were corrected, and a
report line that gave the wrong number of client threads, which was
fixed in `analyze.py`.

Verified in CI, GitHub Actions run 43699662842 on commit `7213aed`: all
six jobs pass, no warnings. The Ubuntu jobs run 178 tests, the macOS
jobs 160. CI runs the tests of the load generator. It does not run the
benchmarks.

Thrown away, and why:

- The first full run on the Mac. The lid was closed two minutes in and
  the machine slept through it. That is what led to the sleep check.
- Two Linux result sets taken with `--poller poll` on both sides.

Not verified:

- **Anything on a real Linux machine or over a network.** The Linux
  numbers are from a virtual machine on a laptop.
- **Why waking workers costs what it seems to.** Counters fit the
  explanation. Nothing was profiled, and the Mac has no such counters.
- **Open-loop latency on the Mac above 25% of capacity.** The client
  could not keep its schedule there.
- **The load generator against another tool.** Its byte counts and
  request counts are tested against the protocol's frame sizes, and its
  rate against the clock. Its latencies have nothing independent to be
  compared with.
- **What sampling costs the server.** A STATS request four times a
  second, taken to be nothing.
- **The power source for the Linux runs.** The container cannot see it.
  The Mac was on battery before them and on mains after.
- **Edge-triggered epoll.** Still not written, so not measured.
- **Runs longer than five seconds.** Nothing here would show a slow
  leak or a pause that comes once a minute. Step 11 has the soak test.

## Step 10: profiling, then optimization (2026-11-15)

What changed: the server was profiled, the profile was written down,
and then five changes were made one at a time, each measured before and
after. Four were kept. Everything is in
[performance.md](performance.md): the profiles, the bottlenecks, every
change with its numbers, and what was looked at and left alone.

Tools: `benchmarks/profile.py` runs the server under the load generator
and looks at it with perf: CPU time by thread and by call with a flame
graph, system calls per request, allocations per request by caller.
`benchmarks/flamegraph.py` draws the graph. `Dockerfile.profile` is the
Linux image with perf. The runner has a suite `hotpath`, seven cells for
before and after.

What the profile showed, for echo of 64 bytes over 100 connections with
four workers in the Linux container: 6.7 system calls per request, over
80% of the event loop's time in the kernel and 5% in the server's own
code. Each response went out by way of the poller (two `epoll_ctl`),
each request was read with two `recv`, and nearly every request woke a
sleeping worker (1.74 `futex` calls against 0.44 with one worker).

The changes:

| | Change | Result |
| --- | --- | --- |
| 1 | Send a response at once, ask the poller only if the socket is full | Kept. Echo, 4 workers: 109,354 to 228,968 |
| 2 | Stop reading when `recv` returns less than asked for | Kept. Echo, 4 workers: 228,968 to 313,335 |
| 3 | Queue requests quietly, wake workers once per turn of the loop | Kept. 100 connections: 220,608 to 316,655 |
| 4 | Send a worker's response without copying it into the output first | Kept. Echo 64 KiB: 16,147 to 20,468 |
| 5 | Read the rest of a large frame straight into the input | Taken out. Echo 64 KiB: 20,468 to 20,541 |

From before to after, Linux container, median of five runs: echo with
one worker 223,437 to 345,928 requests per second, with four 109,354 to
325,481, with eight 100,600 to 320,807. Light compute 170,453 to
259,625. Heavy compute 278 to 289. Echo of 64 KiB 14,045 to 20,468.
System calls per echo request 6.70 to 2.68. Median latency at a fixed
30,000 requests per second 61 to 55 µs.

On the Mac (macOS 26.6.2, Apple clang 17, `poll()`, not pinned, on
battery): echo with one worker 135,232 to 175,305, with four 112,888 to
153,185. Light and heavy compute did not move.

Decisions:

- **perf for all three kinds of profile.** `strace -c` and heaptrack,
  which the brief names, were tried first. strace made the server thirty
  times slower, and a server that slow works in larger batches, so calls
  made once per batch looked rarer than they are. heaptrack could not
  name a call site on this platform.
- **Change 3 was reverted and then put back.** The hotpath suite showed
  no gain, so it came out, as the brief says. Profiles on either side of
  the revert disagreed with the suite, and the `connections` suite run
  on both versions showed 40% to 50% from 10 to 1,000 connections.
  Without the change the server has a fast and a slow way of running,
  and the hotpath suite had caught the fast one ten times out of ten.
  The revert and the re-apply are both in the history.
- **Change 5 stays out though it removed a copy.** It rewrote `Buffer`
  and gained nothing that could be measured.
- **`TCP_NODELAY` is not set.** 200 requests sent in one piece were
  answered as fast without it as with it. Over loopback there is no
  delay for it to remove.
- **Socket buffer sizes are left to the kernel.** One `send` takes a
  whole 64 KiB response.
- **Edge-triggered epoll stays out,** now with a measurement:
  `epoll_wait` is 3% of the event loop's time and `epoll_ctl` is gone
  from the request path.
- **Several event loops were not built.** The profile now says this is
  the next step: the one loop thread is on a CPU 93% of the time or
  more, 70% of it in `send`. It touches everything the loop owns and
  wants a step of its own.
- **`finishWork()` can fail,** because it may send. The server closes
  the connection, as when `flush()` fails.
- **`receive()` now depends on the pollers being level-triggered.**
  This is written where it happens and in architecture.md.

Corrected from step 9: RESULTS.md and this file said that for cheap
requests the event loop thread is full. With four workers it was on a
CPU 71% of the time. Both places say so now.

New tests, 9: one integration test, 30,000 STATS requests in one piece
against a low output watermark, which stalls if requests held back by
full output are not taken up again after a direct send (checked by
removing that line: the connection is cut off after ten seconds). Four
for the pool: one wakeup is enough for fifty queued tasks, a wakeup with
nothing queued, quiet queueing refused and counted like submitting, and
four producers queueing in batches with one wakeup each, checked before
shutdown could hide a lost wakeup. Four for `Connection`: a worker's
response goes straight to the socket, waits behind output already
queued, is queued when the socket does not take it all (8 MiB), and
fails when the peer is gone. Three existing tests of `Connection` were
changed to call `receive()` twice where bytes and the peer's FIN are
both waiting.

Verified on the Mac, at commit `6b31a00`, warnings as errors, no
warnings:

| Configuration | Compiler | Result |
| --- | --- | --- |
| RelWithDebInfo | Apple clang 17 | 168 of 168 tests pass |
| Debug, `ENABLE_ASAN` | Homebrew clang 23.1.5 | 168 of 168 |
| Debug, `ENABLE_TSAN` | Homebrew clang 23.1.5 | 168 of 168 |

Verified in the Linux container (Ubuntu 24.04, g++ 13.3.0), integration
suite on both pollers:

| Configuration | Result |
| --- | --- |
| RelWithDebInfo | 186 of 186 |
| Debug, `ENABLE_ASAN` | 186 of 186 |
| Debug, `ENABLE_TSAN` | 186 of 186 |

Each change was also tested by itself before it was measured: all tests
on the Mac, and all 178 to 190 in the container.

The measurements: eight result sets of the hotpath suite and two of the
connections suite, 27 profiles, none taken while the machine slept
except the one that was deleted.

Verified in CI, GitHub Actions run 43730867830 on commit `ff868a5`: all
six jobs pass on the first run, no warnings. The Ubuntu jobs run 186
tests, the macOS jobs 168.

Thrown away, and why:

- One profile, system calls for light compute, taken just after the
  laptop had slept. It showed a third of the usual throughput. It was
  taken again.
- The result sets for changes 3 and 5 are kept, including the ones that
  showed nothing.

Not verified:

- **Anything on Linux hardware.** The profiles are from a virtual
  machine, where waking an idle CPU may cost more than on hardware, and
  perf has no hardware counters there, only a clock.
- **Why the server without change 3 falls into its slow way of
  running,** and when. It was seen and measured, not explained beyond
  what performance.md says.
- **Why change 5 gained nothing.**
- **Why change 2 gained more than one system call in six explains.**
  Most likely it was the two ways of running again.
- **Why the Mac gains less.** Nothing was profiled on the Mac.
- **`TCP_NODELAY` over a real network.**
- **The 99th percentile at a fixed rate.** In four of the eight result
  sets the load generator fell behind its own schedule in some runs, and
  the tail in those is the client's. The medians are sound.
- **The step 9 suites after the changes.** Only `hotpath` and
  `connections` were run again. The numbers in RESULTS.md describe the
  server before this step.

## Open issues

1. *Resolved in step 4.* Half-close works on macOS. `POLLHUP` is
   handled by reading to the end, and the tests are no longer skipped.
2. **Apple clang sanitizers do not start on the Mac.** With Xcode 16.3
   and Command Line Tools 26.2 on macOS 26.6.2, a TSan binary segfaults
   and an ASan binary hangs before `main`, even for a hello-world.
   Sanitizer builds on this machine need
   `-DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++`.
3. **No test makes a running server's worker throw.** The pieces are
   unit-tested since step 8: a failing request becomes ERROR 6, and a
   throwing task does not stop its worker. The path through the server
   has never run.
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
9. *Resolved in step 4.* After a framing error the server sends FIN and
   reads until the client closes.
10. **The COMPUTE limit bounds iterations, not work.** The cost is
    iterations times data length. The largest request, 100,000 passes
    over about 1 MiB, keeps one worker busy for a long time. Since step
    5 it is cancelled at the end of a shutdown, but not when its own
    connection goes away.
11. *Resolved in step 8.* A request that fails in a worker is answered
    with ERROR 6 and the connection stays open.
12. **`Buffer` keeps its capacity.** After one large frame a connection
    holds that much memory until it closes, even when idle. It is
    bounded by the limits, but not returned.
13. *Resolved in step 4.* The write-stall timeout closes it.
14. **A timeout closes with a plain `close()`.** If input is unread at
    that moment the client sees a reset and not a clean end. No message
    tells the client why it was dropped.
15. **A request with a worker has no time limit.** Together with issue
    10 this means one connection can occupy a worker for a long time.
16. **The partial-frame limit is on the whole frame.** A 1 MiB frame
    over a link slower than about 100 KiB/s cannot finish in the default
    10 s and is cut off.
17. **The write-stall limit sees progress only in steps.** A sender
    learns that the reader has made room when the reader's kernel
    announces it, and macOS announces it about once per half receive
    buffer. Until then a slow reader looks stalled. With the default
    10 s limit and a 4 MiB receive buffer, a client reading slower than
    about 200 KiB/s can be cut off although it is reading steadily.
    This cannot be fixed inside the server, only by choosing the limit.
18. **Requests received but not started are dropped at shutdown without
    a word.** The client sees the connection end after the response to
    an earlier request and has to work out for itself which requests
    were never run.
19. **An exception escaping the event loop ends the process through
    `std::terminate`.** No connection is closed in an orderly way.
20. **Local test runs fail when the Mac sleeps.** Several tests measure
    wall-clock time. If the machine sleeps in the middle of one, the
    test fails or stalls, and connections held open are closed by the
    idle limit because the monotonic clock on macOS keeps counting
    during sleep. Seen on 2026-11-14 with the lid closed overnight.
21. **`EpollPoller::modify()` and `remove()` ignore errors from
    `epoll_ctl`.** For a descriptor that is registered and open they
    can only fail for lack of kernel memory.
22. **A timer entry for a closed connection stays in the heap until its
    time comes**, at most the idle timeout. Memory for it is one small
    entry per connection closed in that window.
23. **A client that ignores "server busy" costs the event loop an
    error frame per request.** Nothing slows such a client down.
24. **`--stats-interval-ms` wakes the loop on every interval** even
    when nothing else is happening.
25. *Resolved in step 10.* Requests are queued without a wakeup and
    the workers are woken once per turn of the loop. Echo with four
    workers does 94% of what it does with one.
26. **No benchmark has run on Linux hardware.** Client and server share
    a laptop, and Linux runs in a virtual machine on it.
27. **On the Mac the load generator is the limit at thousands of
    connections**, because it waits with `poll()` too, and it cannot
    keep an open-loop schedule above a quarter of capacity because
    nothing can be pinned. Those cells are flagged in the reports.
28. **With the defaults, 10,000 busy connections overflow the queue.**
    `--max-connections` is 10,000 and `--queue-capacity` 1,024. In six
    of the fifteen runs at 10,000 connections some requests, 379 to
    2,246 of about 500,000, were answered "server busy".
29. **5,000 connections measured lower than 10,000 on Linux**, 85,693
    against 112,420 with epoll, and the same with `poll()`. Not
    explained.
30. **A report says "plus uncommitted changes" without saying which**,
    and in the container it cannot tell at all, because the image has
    no git.
31. **Three tests of the load generator depend on the machine's
    speed.** The rate kept in an open loop, the coordinated-omission
    comparison and the busy count each need the server to do a certain
    amount in a second or two. Two of them failed once in a sanitizer
    build in step 9 and were given more room. They can fail again on a
    machine that is slow enough.
32. **One event loop thread is the limit for small requests.** After
    step 10 it is on a CPU 93% of the time or more, 70% of that in
    `send`. Several event loops would be the next step. Not built.
33. **For large payloads half of the event loop's time is copying.**
    Two copies per request are left, into the input and out of it into
    the request. Removing the first gained nothing (change 5).
34. **Workers woken together contend for the queue's lock.** 0.65
    `futex` calls per echo request with four workers against 0.26 with
    one. The workers pay for it, not the loop.
35. **`TCP_NODELAY` is not set.** No effect over loopback. Not tested
    over a network, where a client that sends several requests without
    waiting might see small answers held back.
36. **`receive()` relies on level-triggered polling.** It stops at the
    first short read. A poller that reports a socket only once would
    need it to read until the socket is empty again.
37. **A benchmark suite can miss a difference.** The server had two
    ways of running and one suite caught only the faster. Nothing in
    the runner looks for that.
