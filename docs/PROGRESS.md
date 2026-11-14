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

Not verified:

- ERROR 6 from a running server. Nothing a client can send makes a
  worker throw. The conversion is unit-tested in `RequestHandler`, and
  the pool's handling of a throwing task in `ThreadPool`.
- The empty-completion path, for the same reason.
- Whether one allocation per task matters. Nothing was measured.
- The pool under real load. The stress test is a unit test.

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
