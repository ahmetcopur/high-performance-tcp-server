# Architecture

How the server is put together today. The wire format is in
[protocol.md](protocol.md). What has been verified, and what is still
open, is in [PROGRESS.md](PROGRESS.md).

## Modules

Each responsibility has one home.

| Module | Responsibility |
| --- | --- |
| `main.cpp` | Parses the options, installs the signal handler, starts the server. Nothing else. |
| `Config` | The options, their defaults and their allowed ranges. |
| `TcpServer` | The listening socket, the accept logic, the map of connections, which request goes to a worker, and how a shutdown goes. |
| `EventLoop` | The loop itself: the poller, the waker, the completion queue, and when to sleep and for how long. |
| `Poller` | The interface for waiting on descriptors: add, modify, remove, wait. |
| `PollPoller` | `Poller` with `poll()`. Works everywhere. |
| `EpollPoller` | `Poller` with epoll. Linux only, and the default there. |
| `Waker` | Ends the loop's sleep from another thread or a signal handler. An eventfd on Linux, a pipe elsewhere. |
| `TimerQueue` | The deadlines of all connections, as a min-heap. |
| `Connection` | One client: its socket, input and output buffers, state, and deadlines. |
| `Protocol` | Encoding and decoding frames, validating requests, the COMPUTE hash. No sockets. |
| `Buffer` | A byte queue with a read offset. |
| `FileDescriptor` | Owns one descriptor and closes it. The only place that calls `close()`. |
| `ThreadPool` | The worker threads, their bounded task queue, and the counters. |
| `RequestHandler` | What a worker does with a request: ECHO, COMPUTE, and turning a failure into ERROR 6. |

`EventLoop` knows nothing about connections or the protocol. It calls
back into a `Handler`, which `TcpServer` implements, for the four things
that can happen:

1. A shutdown was requested.
2. A worker finished a request.
3. It is time to look at the deadlines, which happens once per
   iteration just before the loop sleeps.
4. A descriptor is ready.

## Ownership

Three rules, and the code is arranged so that breaking one takes
effort.

1. **The event loop thread owns every connection and every buffer.**
   Connections live in a map inside `TcpServer`. Nothing outside the
   loop thread has a pointer or a reference to one.
2. **A worker receives a copy of the request and the id of its
   connection, and nothing else.** The task given to the pool captures
   the decoded frame by value, the descriptor number and the id. It
   cannot reach a `Connection`, a socket, or a buffer.
3. **A result comes back only through the completion queue.** The
   worker calls `EventLoop::post()`, which is the single place where
   the two sides meet, behind one mutex. The loop thread takes the
   result out and looks the connection up again by descriptor and id.

Descriptors have exactly one owner each. A `Connection` owns its
socket, `TcpServer` owns the listening socket and the spare descriptor,
`EventLoop` owns the waker and the poller. Each descriptor is a
`FileDescriptor`, so a descriptor is closed when its owner is destroyed
and at no other time.

## Threads

One thread, the event loop, owns every socket and every connection. It
waits in the poller, accepts, reads, decodes, and writes.

A pool of worker threads runs the requests that cost CPU time (ECHO and
COMPUTE). A worker is given a copy of the request and the id of the
connection it came from. It never sees a socket or a connection object.
When it is done it puts the encoded response on the completion queue and
wakes the event loop through the waker.

The two sides share exactly two things: the pool's task queue and the
completion queue, each behind its own mutex.

## The worker pool

The pool has a fixed number of threads and one queue of tasks. The
queue has a capacity, `--queue-capacity` (default 1024). The capacity
counts tasks that are waiting, not tasks being run.

A task is any callable, including one that can only be moved, which a
request frame captured by value is. It is stored behind a small
interface with one virtual function, one allocation per task. The old
pool wrapped every task in a `shared_ptr` to a `packaged_task` inside a
`std::function` and returned a `std::future` that nobody read.

`trySubmit()` returns false when the queue is full or the pool is
shutting down, and in that case leaves the task untouched. It does not
throw and it does not block.

A task that throws does not end its worker. The pool counts it and
goes on.

### What happens when the queue is full

The request is answered with ERROR 5, "server busy", and the
connection stays open. This is cheap, it is immediate, and the client
learns something it can act on.

The alternative is to stop reading from that connection until the
queue has room, the way a connection is throttled when its output
backs up. The client would then simply be served later, without seeing
an error. It was not chosen, for three reasons:

- The connection would have to be woken when room appears, which needs
  a list of waiting connections and a decision about which goes first.
- A queue that is full for long means the server is behind, and making
  every client wait in silence hides that from them.
- Requests would pile up in socket buffers on both sides, where
  nothing counts them.

The price of the chosen policy is that a client which ignores ERROR 5
and keeps sending costs the event loop one error frame per request.

### What happens when a request fails

A worker that hits an exception while handling a request answers that
request with ERROR 6, "internal error". The connection stays open. If
even the error cannot be built, the worker sends back an empty result
and the event loop closes the connection, so the client is never left
waiting for an answer that will not come.

### Counters

The pool counts tasks submitted, rejected, completed and failed, and
adds up the time its workers spend running tasks. Together with the
queue depth these are what STATS reports and what
`--stats-interval-ms` logs. The submit counters are kept under the
queue's mutex, which is held anyway. The worker counters are atomics,
so a worker finishing a task takes no lock for them.

## Connection ids

Every connection gets a 64-bit id from a counter that only goes up. File
descriptor numbers are reused by the kernel as soon as a socket closes,
so a completion carries both the descriptor and the id. The event loop
drops a completion unless a connection with that descriptor and that id
still exists. This is what makes it safe to close a connection while one
of its requests is still with a worker.

## Connection states

```text
                     accept()
                        |
                        v
                   +----------+   peer sends FIN    +--------------+
                   |   Open   |-------------------->|  ReadClosed  |
                   +----------+                     +--------------+
                        |                             ^        |
                        | length field unusable       |        | nothing left to
                        v                             |        | decode, compute
                   +----------+   peer sends FIN      |        | or send
                   | Closing  |-----------------------+        |
                   +----------+                                v
                        |                                  close()
                        | linger timeout
                        v
                     close()

    From any state: socket error, write failure, or a timeout -> close()
```

| State | Reads | Serves requests | Leaves when |
| --- | --- | --- | --- |
| Open | yes | yes | the peer sends FIN, or a bad length arrives |
| ReadClosed | no | yes, those already received | nothing is left to decode, compute or send |
| Closing | yes, and drops the bytes | no | the peer sends FIN, or the linger timeout passes |

"Closed" is not a state a connection can be in. Closing a connection
closes the descriptor and destroys the object in one step.

### Rules that always hold

1. A connection in ReadClosed is never read again.
2. A connection in Closing never serves another request. Its input
   buffer is empty and stays empty.
3. At most one request per connection is with a worker. Nothing more is
   decoded until its response is queued, so responses leave in the order
   the requests arrived.
4. A connection may be destroyed while its request is with a worker. The
   worker holds no reference to it, and the completion is dropped by the
   id check.
5. A connection in ReadClosed is closed with a plain `close()` only when
   nothing is left unread, so the peer sees FIN and not a reset.
6. Every way out of a state ends in `close()` of the descriptor. The
   tests count the server's descriptors to check this.

### Why Closing does not just close

When a frame's length cannot be used, the server answers with an ERROR
and wants to end the connection. If it called `close()` while the client
was still sending, bytes would be left unread in the socket. The kernel
answers that with RST instead of FIN, and a reset can discard data the
client has not read yet, including the ERROR.

So the server sends the ERROR, calls `shutdown(SHUT_WR)` to send FIN, and
keeps the socket open for reading. Whatever the client still sends is
read and dropped. The connection is closed when the client's own FIN
arrives, or when the linger timeout passes, whichever is first.

## Hangups and errors from poll()

`POLLHUP` does not mean the same thing everywhere.

- **Linux** sets it only when both directions are closed. A peer that
  half-closes with `shutdown(SHUT_WR)` shows up as readable, and `recv()`
  returns 0.
- **macOS** sets it, together with `POLLIN`, as soon as the peer's FIN
  arrives, even with unread data waiting. It reports it only while
  `POLLIN` is requested.

The server therefore never treats `POLLHUP` as "the connection is dead"
on its own:

- While the connection is still being read, `POLLHUP` is handled like
  `POLLIN`. `recv()` collects what is left and then returns 0, which is
  the ordinary path into ReadClosed. Responses still owed are sent.
- When the connection is not being read (ReadClosed), `POLLHUP` means
  the peer is gone in both directions, and the connection is closed.

`POLLERR` and `POLLNVAL` close the connection. For `POLLERR` the actual
error is fetched with `getsockopt(SO_ERROR)` and logged, at most one line
per second.

With epoll the same rules hold, with one addition. epoll has a separate
flag, `EPOLLRDHUP`, for "the peer has finished sending", which `poll()`
on Linux shows only as readable. `EpollPoller` reports it as a hangup,
and asks for it only together with input. A connection in ReadClosed
has stopped reading and may still be sending responses. If
`EPOLLRDHUP` were registered for it, it would be reported on every wait,
look like a hangup while not reading, and the server would close the
connection and drop what it owes. A unit test holds both pollers to
this.

## Pollers

`--poller poll` or `--poller epoll` chooses how the loop waits.

**poll** is given the full list of descriptors on every call, and the
kernel looks at each one. The cost of a wait grows with the number of
connections, whether they are active or not.

**epoll** keeps the list in the kernel. Descriptors are registered
once, changed when their interest changes, and a wait returns only the
ones that are ready. The cost of a wait depends on how many are ready.
It exists only on Linux, where it is the default. The code is compiled
only there.

Three things in the server follow from this:

1. **Interest is changed only when it changes.** With epoll a change is
   a system call. `EpollPoller` remembers what each descriptor is
   registered for and skips the call when nothing would change.
2. **Output is watched only while there is output.** A socket with room
   in its send buffer is always writable. Watching for that with
   nothing to send would wake the loop without pause.
3. **Nothing visits every connection per iteration.** Deadlines come
   from a heap and interest is updated per event, see Timeouts. A scan
   over all connections would cost what epoll saves.

Both pollers are level-triggered: a descriptor is reported for as long
as it is ready. The server may take part of what is there and will be
told again.

### Why not edge-triggered

Edge-triggered epoll reports a descriptor once, when it becomes ready,
and not again until it stops being ready and becomes ready anew. It can
save wakeups, at a price:

- **Everything must be read until `EAGAIN`.** If the server takes part
  of the input and stops, no further event comes for what is left, and
  the connection hangs. The same goes for writing.
- **Reading until `EAGAIN` lets one connection starve the others.** A
  client that sends as fast as the server reads keeps the loop on its
  socket. A fair server has to stop after some amount and remember to
  come back, which means keeping its own list of ready connections.

The server reads at most one frame's worth per event, on purpose, as
part of its input limit. That is exactly what edge-triggered mode does
not allow without the extra bookkeeping. It is left out until a
measurement shows that the saved wakeups matter.

## Timeouts

A connection is always in exactly one phase, decided by what it is
waiting for. Each phase except Busy has a time limit.

| Phase | Meaning | Limit | Option |
| --- | --- | --- | --- |
| Idle | nothing buffered, nothing in flight | 60 s | `--idle-timeout-ms` |
| Partial | a frame has started to arrive and is not complete | 10 s | `--partial-frame-timeout-ms` |
| Busy | a worker has the request | none | |
| Writing | output is waiting for the peer to take it | 10 s | `--write-stall-timeout-ms` |
| Lingering | Closing, our FIN is sent, waiting for the peer's | 2 s | `--linger-timeout-ms` |

The clock for a phase starts when the connection enters it. In Writing
it also restarts whenever some output is sent, so the limit is on a
stall, not on the total time a large response takes.

What each limit is for:

- **Idle** returns the descriptor of a client that connected and went
  away without closing.
- **Partial** stops a client that sends a frame a byte at a time
  (slowloris). More bytes of the same frame do not restart the clock.
  Only completing the frame does.
- **Writing** stops a client that sends requests and never reads the
  responses. Back-pressure already bounds what such a client can make
  the server hold. The timeout takes the connection back.
- **Lingering** bounds how long the server waits for a client that never
  closes its side after an error.

Busy has no limit because the server itself is the slow party there.

A timeout closes the connection with a plain `close()`. If input is
unread at that moment, the client sees a reset. One log line per second
at most names the limit that was reached.

### What the write-stall limit can and cannot see

The server counts output as moving when `send()` accepts bytes. That
happens only when the client's kernel announces that its receive buffer
has room again, and kernels announce it in steps, not byte by byte.
macOS waits until about half the receive buffer is free. Measured on
loopback there, a reader taking 32 KiB every 70 ms lets the sender
write once every 590 ms.

So between two announcements a slow reader and a dead one look the
same. The limit has to be longer than the slowest client you want to
serve needs to read about half its receive buffer. With the default of
10 s and a buffer that has grown to 4 MiB, that is a client reading
about 200 KiB/s.

### How deadlines are kept

Deadlines are in a `TimerQueue`, a min-heap ordered by time. The loop
sleeps until the earliest one.

A connection's deadline changes whenever something happens to it, far
more often than it runs out. Searching the heap on every change would
be the expensive part, so the heap uses lazy deletion: a changed or
cancelled deadline leaves its old entry where it is, and the entry is
thrown away when it reaches the top and is seen to be out of date.

An entry promises only that the connection is due no later than its
time. Deadlines mostly move further away, as when output makes
progress. Then nothing is pushed at all. The entry comes up at the old
time, the connection is asked again, turns out not to be due, and goes
back in with its real deadline. This keeps the heap at about one entry
per connection however busy the connections are.

A connection's deadline and its poll interest are brought up to date
in one place, at the end of `TcpServer::advance()`, which runs after
every socket event and every finished request.

## Shutdown

The first SIGINT or SIGTERM starts an orderly shutdown. A second one
ends the process at once.

### What the first signal does

1. **The listening socket is closed.** New connections are refused.
2. **No new request is started.** Bytes already received but not yet
   decoded are dropped, on every connection.
3. **Connections that are owed nothing are closed** straight away.
4. **Connections that are owed something go to Closing.** That is a
   request with a worker, or a response not fully sent. The request
   finishes, its response is sent, then the server sends FIN and keeps
   reading and dropping input until the client closes. This is the same
   path as after a framing error, for the same reason: a client that is
   still sending must not turn the close into a reset that destroys the
   response it was just sent.
5. **The process exits with code 0** when the last connection is gone.

The whole of this has a deadline, `--shutdown-timeout-ms` (default 5 s),
counted from the signal. When it passes, the remaining connections are
closed as they are, one line is logged, and the process still exits with
code 0. The per-phase timeouts keep running during a shutdown, so a
connection can also be dropped earlier by its own limit.

A client that received its last response and then neither sends nor
closes holds the shutdown for the linger timeout (2 s by default), or
until the deadline if that is sooner.

### What happens to work in progress

A request with a worker is given the chance to finish, up to the
deadline. Requests still running after that belong to connections that
no longer exist. They are told to stop: the COMPUTE loop checks a flag
before every pass over its data, so even the heaviest request gives up
within one pass. Without this, joining the worker threads could hold the
process for minutes after every connection was closed.

### The second signal

The signal handler restores the default action for SIGINT and SIGTERM
after the first signal. A second signal therefore terminates the process
the way an unhandled signal does. Nothing is flushed, and the exit
status is the signal, not 0.

### Order of destruction

The steps at the end of `TcpServer::run()` are written out in the order
they must happen, and do not rely on the order in which members are
declared:

1. Connections are closed, then the listening socket and the spare
   descriptor.
2. Workers are told to give up their work and are joined. Until this
   returns, a worker may still push to the completion queue and use the
   waker.
3. Only then can the loop go, and with it the waker and the completion
   queue. After the join this is the only thread, so what is
   left can be destroyed in any order. The destructor of `TcpServer`
   joins the workers too, for a server that was built and never run.

An exception that escapes the event loop is not caught. The process
ends through `std::terminate` without running any of this.

## Limits

| What | Limit | When it is reached |
| --- | --- | --- |
| Connections | `--max-connections` | The listening socket is no longer watched. New clients wait in the kernel backlog. |
| Descriptors | `ulimit -n` | Each new connection is accepted and closed at once. |
| Frame length | `--max-frame` | ERROR 4, then Closing. |
| Input per connection | one frame of the largest size | The server stops reading from that connection. |
| Requests in flight per connection | 1 | Further requests wait as bytes in the input buffer. |
| Output per connection | `--output-high-water` | The server stops reading and decoding for that connection until the output drains to half. |
| COMPUTE iterations | 100,000 | ERROR 7, the connection stays open. |
| Requests waiting for a worker | `--queue-capacity` | ERROR 5 for that request, the connection stays open. |
| Shutdown | `--shutdown-timeout-ms` | Remaining connections are closed as they are and work in progress is abandoned. |
