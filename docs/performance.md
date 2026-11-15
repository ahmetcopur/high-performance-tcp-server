# Performance

Where the server's time goes, what was changed because of it, and what
each change did. Written in the order it happened: the profile first,
then one change at a time, each measured before and after.

## How it was measured

Everything here was measured in the Linux container on 15 November 2026:

| | |
| --- | --- |
| Machine | Apple M1 laptop, 8 cores, on mains. The last set of profiles, "After the changes", was taken on battery. |
| OS | Ubuntu 24.04.5 in Docker's virtual machine, 8 CPUs, kernel 7.0.14-linuxkit |
| Compiler | g++ 13.3.0 |
| Build for numbers | Release, `-O3 -DNDEBUG` |
| Build for profiles | Release with `-g -fno-omit-frame-pointer` |
| Server | epoll, pinned to CPUs 0 to 3 |
| Load generator | 100 connections, 4 threads, pinned to CPUs 4 to 7, closed loop |

Client and server share the machine and talk over loopback. It is a
virtual machine on a laptop. The numbers say how one version of the
server compares with another on this machine, and not what a server
machine would give.

**Profiles** are made by `benchmarks/profile.py` and kept under
`benchmarks/profiles/`. Each has a `profile.json` with its environment
and command. There are three kinds:

- `cpu`: perf samples the server's threads about 2,000 times a second
  for five seconds and records the call stack each time. The result is
  split into the event loop thread and the workers. `flame.svg` is the
  picture, `stacks.folded` the samples it was drawn from.
- `syscalls`: perf counts the server's system calls at the kernel's
  tracepoints, and the server counts its requests.
- `heap`: perf stops at every `malloc()` and notes who called it.

**Numbers** are from the `hotpath` suite of `benchmarks/run.py`: seven
cells, five runs of five seconds each, median reported. Each set is
under `benchmarks/results/` with a report.

```bash
docker build -t tcpserver-dev . && docker build -t tcpserver-profile -f Dockerfile.profile .
```

```bash
docker run --rm --init --privileged -v "$PWD":/src -e TCPSERVER_COMMIT="$(git rev-parse HEAD)" tcpserver-profile sh -c 'cmake -S /src -B /build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-g -fno-omit-frame-pointer" && cmake --build /build --parallel && python3 /src/benchmarks/profile.py --build /build --tool cpu'
```

```bash
docker run --rm --init -v "$PWD":/src -e TCPSERVER_COMMIT="$(git rev-parse HEAD)" tcpserver-profile sh -c "cmake -S /src -B /build -DCMAKE_BUILD_TYPE=Release && cmake --build /build --parallel && python3 /src/benchmarks/run.py --build /build --suite hotpath --server-cpus 0-3 --client-cpus 4-7"
```

Two tools the brief named were tried and not used. `strace -c` stops the
server at every system call. That made it thirty times slower, and a
server that slow handles far more connections per turn of its loop, so
the calls it makes once per turn looked rarer than they are. heaptrack
counted the allocations but named every call site "??" on this
platform. perf does both jobs without either problem.

## Before any change

The server as it was at the end of step 9, commit `4421641`.

### What one request costs

System calls per request, from
[echo with four workers](../benchmarks/profiles/20261115T095233Z-syscalls-echo-4w/profile.json),
[echo with one](../benchmarks/profiles/20261115T095241Z-syscalls-echo-1w/profile.json)
and
[light compute with four](../benchmarks/profiles/20261115T095249Z-syscalls-light-4w/profile.json):

| System call | Echo, 4 workers | Echo, 1 worker | Light compute, 4 workers |
| --- | ---: | ---: | ---: |
| `epoll_ctl` | 1.97 | 1.96 | 1.97 |
| `recvfrom` | 1.97 | 1.96 | 1.97 |
| `futex` | 1.74 | 0.44 | 0.45 |
| `sendto` | 0.98 | 0.98 | 0.98 |
| `epoll_pwait` | 0.021 | 0.021 | 0.043 |
| `write` (waking the loop) | 0.020 | 0.021 | 0.040 |
| `read` (the loop's wakeup) | 0.013 | 0.014 | 0.032 |
| **All** | **6.70** | **5.39** | **5.47** |

The figures are a percent or two low, because the requests are counted
over a slightly longer stretch than the system calls: where it says
1.97, read 2.

Allocations per request, from
[echo with four workers](../benchmarks/profiles/20261115T095257Z-heap-echo-4w/profile.json):
3.1. One for the request's payload when it is decoded, one for the task
handed to the pool, one for the encoded response, and now and then a
block for the completion queue.

### Where the CPU time goes

Share of each thread's time on the CPU, from
[echo with four workers](../benchmarks/profiles/20261115T095206Z-cpu-echo-4w/profile.json)
([picture](../benchmarks/profiles/20261115T095206Z-cpu-echo-4w/flame.svg)),
[echo with one](../benchmarks/profiles/20261115T095214Z-cpu-echo-1w/profile.json)
([picture](../benchmarks/profiles/20261115T095214Z-cpu-echo-1w/flame.svg))
and
[light compute with four](../benchmarks/profiles/20261115T095223Z-cpu-light-4w/profile.json)
([picture](../benchmarks/profiles/20261115T095223Z-cpu-light-4w/flame.svg)).

The event loop thread:

| | Echo, 4 workers | Echo, 1 worker | Light compute, 4 workers |
| --- | ---: | ---: | ---: |
| Requests per second while profiled | 102,187 | 228,563 | 181,965 |
| Time on a CPU, in cores | 0.71 | 1.00 | 0.92 |
| of it in the kernel | 84% | 82% | 82% |
| `send` | 45.3% | 53.8% | 51.9% |
| waking a worker (`pthread_cond_signal`) | 21.7% | 6.0% | 6.2% |
| `recv` | 12.6% | 15.0% | 15.2% |
| `epoll_ctl` | 11.6% | 15.3% | 15.9% |
| the server's own code | 4.9% | 6.0% | 5.2% |
| `epoll_wait` | 1.7% | 1.8% | 2.9% |
| `malloc` and `free` | 1.2% | 1.2% | 1.3% |
| mutexes | 0.7% | 0.7% | 0.8% |

All workers together:

| | Echo, 4 workers | Echo, 1 worker | Light compute, 4 workers |
| --- | ---: | ---: | ---: |
| Time on a CPU, in cores | 0.27 | 0.14 | 1.93 |
| going to sleep and waking (`pthread_cond_wait`) | 57.7% | 18.9% | 2.5% |
| the server's own code | 23.3% | 37.5% | 93.6% |
| `malloc` and `free` | 6.5% | 13.8% | 1.2% |
| reading the clock | 6.1% | 16.2% | 1.7% |
| mutexes | 5.3% | 10.2% | 0.7% |

### What it shows

**1. The parsing, the buffers and the protocol are not where the time
goes.** The event loop spends 5% to 6% of its time in the server's own
code and over 80% in the kernel. Whatever is to be gained is in how
often the kernel is asked for something.

**2. Every response makes a detour through the poller.** A response is
put in the connection's buffer, the poller is told to report when the
socket can be written (`epoll_ctl`), the loop goes round, the poller
reports it, the response is sent, and the poller is told to stop
(`epoll_ctl` again). That is two system calls per request, 12% to 16% of
the event loop's time, and a turn of the loop between a response being
ready and its being sent. The socket could nearly always have taken the
response at once.

**3. Every request is read twice.** The loop reads until the socket says
there is nothing more, so each request costs one `recv` that brings the
request and one that brings nothing. `recv` is 13% to 15% of the event
loop's time, and about half of that is the second call.

**4. With idle workers, every request wakes one.** With four workers on
echo, a request costs 1.74 `futex` calls, against 0.44 with one worker.
The event loop spends 21.7% of its time waking workers, against 6.0%,
and the workers spend 57.7% of theirs going to sleep and waking, to do
work that takes a fraction of that. This is what step 9 inferred from
the context switches. A worker that finds the queue empty sleeps, and
with four of them sharing the requests each one nearly always finds it
empty.

**5. Waking another thread is the dearest single thing the loop does.**
Of the event loop's samples in the first profile, 17% are in the
kernel's `try_to_wake_up` on behalf of a worker, and 15% are in
`sock_def_readable`, which is `send` telling the client's thread that
its response has arrived. Together a third of the event loop's time on
the CPU. The second of these cannot be avoided: the client has to be
told. In a virtual machine, waking a CPU that is idle may cost more than
on hardware. That was not measured.

**6. `send` is half of the loop's time, and most of it is not the
server's to save.** On loopback, `send` runs the whole of TCP for both
ends on the sender's thread: the segment goes out, comes back in, is
put in the client's socket, and the client is woken, before `send`
returns. There is already one `send` per response.

**7. The event loop is not always the limit, and step 9 said it was.**
With one worker the event loop is on a CPU all the time, and it is the
limit. With four workers it is on a CPU 71% of the time and the server
does half as much. Nothing in the server is full. What limits it is that
the whole system works in turns: the loop takes about 48 requests from
each `epoll_wait` (0.021 calls per request), hands them over, sleeps
until the answers and the next requests are there, and takes the next
48. A request waits for the turns before it, and a turn takes as long as
everything done in it. So work added to each request on the loop, such
as waking a worker, slows the server down even while the loop has time
to spare. RESULTS.md said "adding workers cannot help while that thread
is full". The thread is not full, and that sentence has been corrected.

**8. Allocation is not a bottleneck.** Three allocations per request
cost the event loop 1.2% of its time. For the workers on echo it is
6.5% of 0.27 cores.

## The changes

Each was measured with the `hotpath` suite. The starting figures are in
[the first report](../benchmarks/results/20261115T095329Z-hotpath-linux-epoll/report.md):

| Cell | Requests/s | Median latency | 99th percentile |
| --- | ---: | ---: | ---: |
| Echo 64 B, 1 worker | 223,437 | 412 µs | 881 µs |
| Echo 64 B, 4 workers | 109,354 | 872 µs | 1.84 ms |
| Echo 64 B, 8 workers | 100,600 | 975 µs | 1.91 ms |
| Light compute, 4 workers | 170,453 | 532 µs | 1.25 ms |
| Heavy compute, 4 workers | 278 | 361 ms | 366 ms |
| Echo 64 KiB, 4 workers | 14,045 | 7.05 ms | 10.35 ms |
| Echo 64 B, 4 workers, fixed 30,000 requests/s | 30,000 | 61 µs | 159 µs |

The first six are closed loops: their latency is mostly the time a
request waits behind the others. The last is an open loop with two
client threads, and its latency is what a request costs when the server
is not pushed.

Requests per second after each change, each column one result set:

| Cell | Before | 1 | 2 | 3 | 4 | 5 (taken out) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Echo 64 B, 1 worker | 223,437 | 326,836 | 343,381 | 345,250 | 345,928 | 352,500 |
| Echo 64 B, 4 workers | 109,354 | 228,968 | 313,335 | 323,879 | 325,481 | 324,866 |
| Echo 64 B, 8 workers | 100,600 | 224,732 | 311,140 | 307,459 | 320,807 | 321,065 |
| Light compute, 4 workers | 170,453 | 250,076 | 257,608 | 251,569 | 259,625 | 252,965 |
| Heavy compute, 4 workers | 278 | 285 | 289 | 284 | 289 | 282 |
| Echo 64 KiB, 4 workers | 14,045 | 13,648 | 16,107 | 16,147 | 20,468 | 20,541 |
| Fixed 30,000/s: median latency | 61 µs | 62 µs | 58 µs | 55 µs | 55 µs | 54 µs |

Reports, in the same order:
[before](../benchmarks/results/20261115T095329Z-hotpath-linux-epoll/report.md),
[1](../benchmarks/results/20261115T100557Z-hotpath-linux-epoll/report.md),
[2](../benchmarks/results/20261115T101403Z-hotpath-linux-epoll/report.md),
[3](../benchmarks/results/20261115T102245Z-hotpath-linux-epoll/report.md),
[4](../benchmarks/results/20261115T104833Z-hotpath-linux-epoll/report.md),
[5](../benchmarks/results/20261115T105745Z-hotpath-linux-epoll/report.md).

The five runs of a cell differ by up to 15% of its median. A difference
smaller than a cell's own spread is not a difference.

### 1. Send a response at once. Kept.

Commit `eea9049`. Bottleneck 2 above. `advance()` now sends what a
connection owes as soon as it has taken its requests, and asks the
poller only when the socket does not take everything.

Echo with four workers doubled, from 109,354 to 228,968. With one worker
it went from 223,437 to 326,836, and light compute from 170,453 to
250,076. Heavy compute and large payloads did not move, as expected:
neither is limited by system calls on the loop.

One thing needed care. Requests are held back while a connection's
output is over its high watermark. If sending at once brings the output
back under it, the requests have to be taken up again there and then,
because the client may have sent everything and no event would bring
them up later. The test for it sends 30,000 STATS requests in one piece
to a server with a low watermark. With that second look removed, the
connection stalls and is cut off after ten seconds.

### 2. One `recv` per event. Kept.

Commit `114c8b8`. Bottleneck 3. A `recv` that returns less than it was
asked for has emptied the socket, and `receive()` now stops there. This
leans on the pollers being level-triggered: if more arrives, or the peer
closes, the socket is reported again.

Echo with four workers went from 228,968 to 313,335 and with eight from
224,732 to 311,140. Echo of 64 KiB went from 13,648 to 16,107. With one
worker, 326,836 to 343,381, the two sets of runs overlap.

The gain with four workers is larger than one system call in six
explains. See change 3: the server was moving between two ways of
running, and this result set caught the faster one.

### 3. Wake the workers once per turn. Kept, after being taken out.

Commit `43450c9`, reverted in `56eeaaf`, put back in `fe5ac43`.
Bottleneck 4. The loop queues requests without waking anyone
(`tryQueue()`), and before it waits it wakes as many sleeping workers as
there are requests waiting (`wakeWorkers()`). No answer is later for it:
only the loop can send one, and it looks at finished work at the start
of its next turn, which comes after the wakeup either way.

The hotpath suite showed nothing: 313,335 to 323,879 with four workers,
inside the spread. By the rule for this step it was reverted.

Profiles taken on either side of the revert said otherwise:

| Echo, 4 workers, while profiled | Without | With |
| --- | ---: | ---: |
| Requests per second | 228,967 | 316,254 |
| `futex` calls per request | 1.19 | 0.62 |
| Event loop's time spent waking workers | 11.5% | 0.2% |

So the `connections` suite was run on both versions, one straight after
the other
([without](../benchmarks/results/20261115T102933Z-connections-linux-epoll/report.md),
[with](../benchmarks/results/20261115T103319Z-connections-linux-epoll/report.md)):

| Connections | Without | With |
| ---: | ---: | ---: |
| 1 | 14,548 | 14,030 |
| 10 | 74,643 | 104,379 |
| 100 | 220,608 | 316,655 |
| 1,000 | 215,500 | 320,555 |
| 5,000 | 165,425 | 200,719 |
| 10,000 | 150,263 | 202,701 |

At 100 connections the runs without the change lie between 157,336 and
246,195, and with it between 301,294 and 323,110.

Without the change the server has two ways of running. If every turn of
the loop brings enough requests, the workers never get to sleep, nobody
needs waking, and it is fast. That is what the hotpath suite caught, in
ten runs out of ten. If the workers do get to sleep, every request wakes
one, the loop is slower for it, each turn brings fewer requests, and it
stays that way. Which of the two it settles in depends on small things:
perf watching was enough. With the change, what waking costs does not
depend on which way the server is running, so there is one way left.

The lesson for the method: one suite that shows no difference does not
show that there is none.

### 4. Send a worker's response from where it is. Kept.

Commit `dbfc0ad`. Found by profiling large payloads, which the first
profiles had not covered
([profile](../benchmarks/profiles/20261115T104350Z-cpu-large-4w/profile.json)).
With 64 KiB payloads the event loop is on a CPU all the time and spends
65% of it copying bytes, in three places of about equal weight: from the
read buffer into the connection's input, from the input into the
request, and from the worker's response into the connection's output.

`finishWork()` now offers the response to the socket first, if no other
output is waiting, and copies only what the socket does not take.
Responses the loop makes itself are still collected and sent together,
so a thousand PINGs in one piece are answered in a few sends.

Echo of 64 KiB went from 16,147 to 20,468. Nothing else moved.

### 5. Read large frames straight into the input. Taken out.

Commit `80b3597`, reverted in `9eaf379`. The second of the three copies.
Once the start of a frame is in, its length is known, and the rest was
read to where it belongs and not by way of a buffer on the stack.
`Buffer` was rewritten for it, to hand out room to be filled in place.

Echo of 64 KiB: 20,468 before, 20,541 after. No cell moved. The copy was
gone from the code and the time was not gone from the server. Why was
not looked into. A likely reason is that the bytes have to be brought
into the processor's cache once either way, by the copy or by whoever
touches them next. A rewritten `Buffer` for nothing is a bad trade, so
it went.

### Looked at and not changed

**`TCP_NODELAY`.** Nagle's algorithm can hold a small response back
while an earlier one is unacknowledged, which would hit a client that
sends several requests without waiting. A probe sent 200 ECHO requests
in one piece and timed the answers, which the server sends one by one.
In the Linux container the 200 took 10 to 11 ms without the option and
11 to 12 ms with it. On the Mac they took 2.6 ms without it, and with it
was not tried. No answer waited anywhere near the 40 ms such a hold-up
lasts. Over loopback there is
nothing for the option to fix. Over a real network there might be. That
was not tested, and the option is not set.

**Socket buffer sizes.** With 64 KiB payloads the server makes 0.97
`sendto` calls per request and never asks the poller to wait for room
([counts](../benchmarks/profiles/20261115T104342Z-syscalls-large-4w/profile.json)).
One `send` takes a whole response. Linux sizes the buffers itself, up to
4 MiB here, and setting a size by hand turns that off.

**Edge-triggered epoll.** It saves reports for sockets that stay ready.
After changes 1 and 2 `epoll_wait` is 3% of the event loop's time and
`epoll_ctl` is not called at all on the request path, so there is little
left for it to save. Change 2 also depends on level-triggered reports.

**Allocations.** Still 3.1 per request. `malloc` and `free` are 2% of
the event loop's time.

**The clock in the workers.** A worker reads the clock twice per task to
keep the busy-time counter, 18% to 21% of the workers' time on echo.
That is a fifth of 0.2 cores, on threads that are not the limit.

**CPU affinity inside the server.** The benchmark pins the server as a
whole to four CPUs. Pinning its threads one by one was not tried. The
server's threads are forced off a CPU a few hundred times a second, so
they are not fighting each other for one.

**Answering ECHO on the event loop.** It would remove the hand-over for
the cheapest request. It would also remove what the echo benchmark is
there to measure, which is the cost of that hand-over.

**Several event loops.** See the next section. This is the change the
profile now asks for, and it was not made.

## After the changes

Commit `6b31a00`. The request path is that of `dbfc0ad`, the last change
that was kept.

### What one request costs now

System calls per request, before and after
([echo, four workers](../benchmarks/profiles/20261115T143541Z-syscalls-echo-4w/profile.json),
[echo, one](../benchmarks/profiles/20261115T143549Z-syscalls-echo-1w/profile.json),
[light compute](../benchmarks/profiles/20261115T150940Z-syscalls-light-4w/profile.json)):

| System call | Echo, 4 workers | Echo, 1 worker | Light compute, 4 workers |
| --- | ---: | ---: | ---: |
| `epoll_ctl` | 1.97 → 0 | 1.96 → 0 | 1.97 → 0 |
| `recvfrom` | 1.97 → 0.98 | 1.96 → 0.98 | 1.97 → 0.98 |
| `futex` | 1.74 → 0.65 | 0.44 → 0.26 | 0.45 → 0.22 |
| `sendto` | 0.98 → 0.98 | 0.98 → 0.98 | 0.98 → 0.98 |
| `epoll_pwait` | 0.021 → 0.026 | 0.021 → 0.029 | 0.043 → 0.070 |
| **All** | **6.70 → 2.68** | **5.39 → 2.27** | **5.47 → 2.39** |

A request now costs one `recv`, one `send`, and a share of the calls made
once per turn of the loop.

Allocations per request are unchanged at 3.1.

### Where the event loop's time goes now

From [echo with four workers](../benchmarks/profiles/20261115T143504Z-cpu-echo-4w/profile.json)
([picture](../benchmarks/profiles/20261115T143504Z-cpu-echo-4w/flame.svg)),
[echo with one](../benchmarks/profiles/20261115T143513Z-cpu-echo-1w/profile.json),
[light compute](../benchmarks/profiles/20261115T143522Z-cpu-light-4w/profile.json)
and [64 KiB payloads](../benchmarks/profiles/20261115T143532Z-cpu-large-4w/profile.json):

| | Echo, 4 workers | Echo, 1 worker | Light compute | Echo 64 KiB |
| --- | ---: | ---: | ---: | ---: |
| Requests per second while profiled | 304,049 | 317,535 | 232,186 | 16,839 |
| Time on a CPU, in cores | 0.93 | 0.95 | 0.96 | 0.99 |
| `send` | 70.2% | 71.9% | 70.1% | 26.4% |
| `recv` | 12.9% | 12.7% | 14.0% | 14.7% |
| the server's own code | 9.9% | 8.0% | 6.6% | 52.4% |
| `epoll_wait` | 3.3% | 3.4% | 5.3% | 1.1% |
| `malloc` and `free` | 2.1% | 2.7% | 1.6% | 5.1% |
| mutexes | 1.1% | 0.9% | 1.2% | 0.2% |
| waking a worker | 0.1% | 0% | 0% | 0% |
| `epoll_ctl` | 0% | 0% | 0% | 0% |

Before, with four workers on echo, the event loop was on a CPU 71% of
the time and a fifth of that went on waking workers. Now it is on a CPU
93% of the time or more in every case, and does three times as much.

### What limits the server now

**The one event loop thread, and in it, `send`.** Seven tenths of the
loop's time is the kernel carrying a response through TCP to the client
on the same machine. That is one `send` per response, and the server has
no cheaper way to hand over a response.

The next step would be several event loops, each with its own poller and
its own share of the connections, so that more than one thread can be in
`send` at once. It was not done in this step. It touches everything the
loop owns, which is every connection, the timers, the accept path and
the shutdown, and it wants its own step with its own tests. On this
machine it could also not be measured well: the load generator shares
the eight cores with the server.

**For large payloads, copying.** With 64 KiB payloads half of the
loop's time is still the two copies that are left, into the input and
out of it into the request. Change 5 removed the first and gained
nothing.

**The workers are not a limit for cheap requests.** With four workers on
echo they use 0.21 cores together. They do contend for the queue's
lock when they are woken together: 0.65 `futex` calls per request with
four workers against 0.26 with one. That is paid by the workers, and
the loop spends 1% of its time on locks.

## The same changes on the Mac

The hotpath suite on macOS 26.6.2, Apple clang 17, Release, `poll()` on
both sides, nothing pinned, on battery. Before is commit `4421641`,
after is `6b31a00`, measured one straight after the other
([before](../benchmarks/results/20261115T151010Z-hotpath-macos-poll/report.md),
[after](../benchmarks/results/20261115T151421Z-hotpath-macos-poll/report.md)).

| Cell | Before | After |
| --- | ---: | ---: |
| Echo 64 B, 1 worker | 135,232 | 175,305 |
| Echo 64 B, 4 workers | 112,888 | 153,185 |
| Echo 64 B, 8 workers | 106,712 | 149,803 |
| Light compute, 4 workers | 117,520 | 119,538 |
| Heavy compute, 4 workers | 291 | 286 |
| Echo 64 KiB, 4 workers | 35,167 | 38,704 |
| Fixed 30,000/s: median latency | 196 µs | 164 µs |

Echo gains 30% to 40%, where Linux gained 55% to 220%. Light compute
does not move on the Mac. Echo of 64 KiB is up 10%, with runs between
31,789 and 39,606 after and between 34,545 and 36,126 before, so that
one is not certain. Nothing was profiled on the Mac, so why the gains
are smaller there is not known. One reason may be that with `poll()`
changing what is watched was never a system call of its own, so change 1
had less to remove.

## In short

On Linux, in the container, from before step 10 to after:

| | Before | After |
| --- | ---: | ---: |
| Echo, 1 worker, requests/s | 223,437 | 345,928 |
| Echo, 4 workers, requests/s | 109,354 | 325,481 |
| Echo, 8 workers, requests/s | 100,600 | 320,807 |
| Light compute, 4 workers, requests/s | 170,453 | 259,625 |
| Heavy compute, 4 workers, requests/s | 278 | 289 |
| Echo 64 KiB, requests/s | 14,045 | 20,468 |
| Echo, 4 workers, 100 connections: median latency | 872 µs | 303 µs |
| Echo at a fixed 30,000 requests/s: median, 99th percentile | 61 µs, 159 µs | 55 µs, 94 µs |
| System calls per echo request, 4 workers | 6.70 | 2.68 |

Step 9 found that adding workers made echo slower: four workers did
half of what one did. Four now do 94% of what one does.
