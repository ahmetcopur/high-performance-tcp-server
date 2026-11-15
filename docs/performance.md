# Performance

Where the server's time goes, what was changed because of it, and what
each change did. Written in the order it happened: the profile first,
then one change at a time, each measured before and after.

## How it was measured

Everything here was measured in the Linux container on 15 November 2026:

| | |
| --- | --- |
| Machine | Apple M1 laptop, 8 cores, on mains |
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
