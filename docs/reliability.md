# Reliability

How the server is shown to survive bad input, bad clients and a long
run, and what each of those tests can and cannot say.

There are three kinds of test beyond the unit and integration tests:

| | What it looks for | Where | How long |
| --- | --- | --- | --- |
| Fuzzing | Input that crashes the decoder, or that a connection handles differently depending on how it arrives | `tests/fuzz/` | A minute per target in CI, longer by hand |
| Stress and fault tests | A server that misbehaves, or keeps something, when clients do | `tests/test_stress.py` | 20 seconds, part of `ctest` |
| Soak test | A slow loss of memory or descriptors | `benchmarks/soak.py` | 30 minutes by hand, 30 seconds in `ctest` |

## Fuzzing

A fuzzer runs a function over and over with input it makes up. It
watches which branches each input reaches, keeps the inputs that reach
something new, and changes those further. It stops when the function
crashes, when a sanitizer reports something, or when a check in the
harness fails, and saves the input that did it.

Two targets, built with libFuzzer, AddressSanitizer and UBSan:

**`fuzz_decode`** gives arbitrary bytes to `decodeFrame()`. It does not
only wait for a crash. For every input it checks that the answer agrees
with the bytes:

- which of the three outcomes it is (complete, incomplete, invalid), by
  the length field and the number of bytes alone,
- that a complete frame consumed exactly its length,
- that encoding the decoded frame gives the same bytes back,
- that a request judged valid has the version, the reserved field and a
  type that make it so,
- that what a worker answers to it is one whole frame, with the same
  request id, of the response type or an ERROR.

**`fuzz_stream`** gives a byte stream to a real `Connection` on a socket
pair, driven the way the server drives one. The first byte of the input
says how to cut the rest into pieces, 1 to 127 bytes each or all at
once, and whether the "worker" answers at once or a turn later. What the
client receives is compared with a second reading of the same bytes:
decoded in one piece, by the plain rules in [protocol.md](protocol.md).

Where the stream is cut and when the answers arrive must not change
which responses the client gets, or their order. The connection must
also reach its end: a harness that runs out of turns reports a
connection that is stuck.

### Running it

It needs clang, which libFuzzer is part of. On the Mac that is the
Homebrew one.

```bash
cmake -B build-fuzz -DENABLE_FUZZING=ON -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
```

```bash
cmake --build build-fuzz --parallel --target fuzz_decode fuzz_stream
```

```bash
mkdir -p found && build-fuzz/fuzz_stream -max_total_time=300 -max_len=2048 found tests/fuzz/corpus/stream
```

New inputs go to the first directory. The second is the corpus in the
repository, which is only read. To keep what a run found, merge it in:
`-merge=1` copies only the inputs that reach something the corpus did
not.

```bash
build-fuzz/fuzz_stream -merge=1 -max_len=2048 tests/fuzz/corpus/stream found
```

A failure leaves a file named `crash-…` in the current directory.
Running the target with that file as its only argument repeats it.

### The corpus

`tests/fuzz/make_corpus.py` writes the inputs to start from: one frame
of each type, each way a frame can be wrong, frames cut short, and for
the stream target pipelines with errors in between, each cut up six
ways. That is 23 inputs for the decoder and 54 for the stream. The
fuzzers added 22 and 143 that reach more.

### What was run

On the Mac (Apple M1, macOS 26.6.2, Homebrew clang 23.1.5, RelWithDebInfo
with ASan and UBSan), 15 November 2026, at commit `9718a2e` plus the
harness:

| Target | Time | Inputs tried | Failures |
| --- | --- | ---: | ---: |
| `fuzz_decode` | 5 minutes | 194,296,437 | 0 |
| `fuzz_stream` | 15 minutes | 1,505,745 | 0 |

The stream target is slower because every input opens a socket pair and
makes a system call for every piece.

The stream run reported one input as slow, 933 seconds. The laptop's
lid had been closed while it ran. The same input takes less than a
second.

CI fuzzes each target for a minute on every push, on Ubuntu with its
clang. In the first such run that was 27,709,574 inputs for the decoder
and 128,725 for the stream target, with no failure.

### Do the targets find anything?

A fuzzer that reports nothing has either found no bug or cannot find
one. To tell which, two bugs were planted, one at a time, and taken out
again:

| Planted bug | Target | Found after |
| --- | --- | --- |
| The decoder calls a frame that exactly fits its bytes incomplete | `fuzz_decode` | about 1,300 inputs, under a second |
| A connection takes frames out of its input while a request is with a worker | `fuzz_stream` | a few seconds |

### What fuzzing does not cover here

- The event loop, the pollers, the thread pool and the timers. The
  stream target drives a `Connection` by hand.
- Time. Nothing in either target waits, so no timeout is ever reached.
- What `COMPUTE` computes. The decoder target runs it only when it is
  quick, and the stream target answers with the request's payload.
- Frames above 256 bytes in the stream target and 1,024 in the decoder
  target. The limits are set low so that the fuzzer reaches them.

## Stress and fault tests

`tests/test_stress.py`, run by `ctest` as `stress_poll`, and as
`stress_epoll` on Linux. Each test is one kind of abuse:

| Test | What the clients do | What must hold while they do |
| --- | --- | --- |
| Connection burst | 300 connect at once against a limit of 50 | All 300 are served. The server reaches 50 connections and never has 51 |
| Churn | 2,400 connections leave in eight ways: at once, mid-frame, with a request in flight, by reset, inside a large frame, after an abandoned pipeline, after a half-close | Two well-behaved clients get every answer right |
| Slow readers | 20 send 64 KiB requests and never read | Each is stopped from sending, then cut off. Memory stays within the limits. Four well-behaved clients are served |
| Slowloris | 200 send one byte every 200 ms and never finish a frame | Every one is cut off, none sooner than four fifths of the limit and none more than three seconds after it |
| Queue saturation | 40 send COMPUTE without pause to 2 workers and a queue of 4 | Every answer is the right result or "server busy". PING stays under two seconds. The server's count of refusals equals the clients' |
| Descriptor exhaustion | 600 connect while every descriptor is in use, and 300 more once they are free | With none free, all 600 are turned away at once and none is left hanging. Those already in are served. Afterwards newcomers are served |
| Shutdown under load | 50 keep asking while the server is told to stop | Every answer received is whole and right, and every connection ends between two frames. Exit code 0 |
| Restarts | 25 times: eight COMPUTE requests under way, then SIGINT or SIGTERM | All eight are answered, then the connection ends without a reset. Exit code 0, and the next start gets the port |
| Everything at once | 24 misbehave in six ways for six seconds | Six well-behaved clients get every answer right |

### What is checked afterwards

The brief asks that shutdown and cleanup are verified explicitly. Every
test except the two about stopping ends with the same two steps, and
those two check stopping in their own way.

**Nothing is left behind.** The server is still running. Its number of
open descriptors is back to what it was before the test. It answers an
echo. By its own counters it has one connection, the one asking, nothing
in its queue, as many requests finished as handed over, and none that
failed in a worker.

**It stops cleanly.** Sent SIGTERM, it exits with code 0 within fifteen
seconds, and its port can be bound again at once.

To see that these checks bite, a bug was planted and taken out again: a
descriptor leaked on every failed read. The churn test reported 882
descriptors where 7 were expected.

### What had to be fixed in the tests

Two tests first relied on timing to bring about the state they are
about, and it did not always come about. A test that passes because the
hard case never happened proves nothing, so each now makes sure of it:

- The descriptor test connected 300 clients at once and expected some to
  be refused. In one run of four the server kept up and refused none.
  It now holds connections open until one more is turned away.
- The burst test expected to see the server reach its limit of 50. On
  Linux it served them too fast and peaked at 42. The first clients in
  now stay until the limit has been seen.

No test in this file found a bug in the server.

## Soak test

`benchmarks/soak.py` keeps the server under mixed load and looks for a
slow loss of memory or descriptors.

```bash
python3 benchmarks/soak.py --build build-release --minutes 30
```

The time is cut into cycles of a minute. In each cycle the server gets,
all at once: echo of 64 bytes over 20 connections as fast as they go,
200 connections that say nothing, echo of 64 KiB over 5 connections,
light COMPUTE over 10, PING over 5, and two clients that misbehave in
seven ways. The server's idle timeout is set to half a cycle, so the
200 silent connections run into it every time.

At the end of each cycle every client is gone, and the server is looked
at while idle. Only then can two readings be compared: under load, what
the server holds depends on what is in flight.

It passes if, leaving the first three cycles out as warm-up:

- the open descriptors at the end of every cycle are what they were
  before the first,
- the memory at the end of the cycles does not grow. Two questions, and
  both must be answered no: is the median of the last third more than
  2 MiB or 5% above the median of the first third, and does a straight
  line fitted through all of them rise by more than that,
- no well-behaved request failed, timed out, or lost its connection,
- every idle connection was closed by the idle timeout, and no
  connection that was in use,
- the server is still running and exits with code 0 when told to stop,
- the machine did not sleep in the middle of a cycle.

### Which memory

On Linux, resident memory plus what was swapped out. On macOS, the
footprint, as the `footprint` tool reports it.

Resident memory alone hides a leak on macOS. Leaked memory is never
touched again, so the system compresses it, and compressed memory is
not counted as resident. This was found by trying the test out on a
server with a leak of 16 bytes per request planted in it:

| Cycle | Resident memory | Footprint |
| ---: | ---: | ---: |
| 2 | 12.3 MiB | 12.2 MiB |
| 3 | 16.4 MiB | 16.6 MiB |
| 4 | 20.5 MiB | 20.8 MiB |
| 5 | 24.4 MiB | 24.9 MiB |
| 6 | 7.4 MiB | 29.0 MiB |

Measured by resident memory, the leaking server passed. Measured by
footprint it fails, with 16.75 MiB of growth in half a minute. The two
columns are from two runs of the same binary.

### Results

Two runs of 30 minutes on 15 November 2026, side by side on one Apple
M1 laptop, both of Release builds of commit `cedb89e`
([Mac](../benchmarks/results/20261115T201441Z-soak-macos-poll/report.md),
[Linux](../benchmarks/results/20261115T201448Z-soak-linux-epoll/report.md)):

| | Mac | Linux container |
| --- | ---: | ---: |
| OS, compiler, poller | macOS 26.6.2, Apple clang 17, `poll()` | Ubuntu 24.04 in Docker, g++ 13.3.0, epoll |
| Verdict | passed | passed |
| Requests answered | 139,081,589 | 118,434,528 |
| Descriptors at the end of each of 30 cycles | 7, as at the start | 7, as at the start |
| Memory at the end of a cycle, after warm-up | 2.70 to 4.47 MiB | 4.98 to 5.17 MiB |
| Median of the first third, of the last third | 4.27, 4.39 MiB | 5.04, 5.04 MiB |
| Rise of the fitted line over the run | +0.94 MiB | -0.03 MiB |
| Most memory seen under load | 4.5 MiB | 5.9 MiB |
| Most descriptors seen under load | 250 | 249 |
| Idle connections closed by the idle timeout | 6,000 of 6,000 | 6,000 of 6,000 |
| Requests that failed in a worker | 0 | 0 |
| Exit code when told to stop | 0 | 0 |

Each report has the reading after every cycle and a plot of them.

**The two runs before these said FAILED, and are kept**
([Mac](../benchmarks/results/20261115T194251Z-soak-macos-poll/report.md),
[Linux](../benchmarks/results/20261115T194258Z-soak-linux-epoll/report.md)).
Memory and descriptors were flat in both, over 115 and 122 million
requests. They failed on an expectation in the test that was wrong. The
200 idle connections sat for the whole minute of a cycle, the server's
idle timeout was a minute, the server closed them as it should, and the
test counted 6,000 lost connections. The test now sets the timeout to
half a cycle and demands that every idle connection is closed by it.

### What the soak test does not show

- Half an hour is not a week. A leak of a few bytes per thousand
  requests would still be inside the allowance.
- The load is the same every minute. Something that only goes wrong
  after a rare sequence of events may never be asked for.
- Both runs shared one laptop, so each had about half of it.
- Under a sanitizer memory is reported and not judged. AddressSanitizer
  keeps freed memory back on purpose, 380 MiB of it in the short run
  that `ctest` does, and how fast that fills has nothing to do with the
  server. Descriptors are judged in every build.

## What none of this covers

- A real network: loss, delay, reordering, a peer that vanishes without
  a reset.
- The machine running out of memory.
- Clocks that jump.
- Several servers, or a server restarted while clients keep their
  connections.
