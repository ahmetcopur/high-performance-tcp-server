# Architecture

How the server is put together today. The wire format is in
[protocol.md](protocol.md). What has been verified, and what is still
open, is in [PROGRESS.md](PROGRESS.md).

## Threads

One thread, the event loop, owns every socket and every connection. It
waits in `poll()`, accepts, reads, decodes, and writes.

A pool of worker threads runs the requests that cost CPU time (ECHO and
COMPUTE). A worker is given a copy of the request and the id of the
connection it came from. It never sees a socket or a connection object.
When it is done it puts the encoded response on the completion queue and
wakes the event loop through a pipe.

The two sides share exactly two things: the pool's task queue and the
completion queue, each behind its own mutex.

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

The event loop finds the nearest deadline by looking at every connection
once per iteration and passes it to `poll()` as its timeout. This is a
linear scan, which adds nothing while the poll set is rebuilt every
iteration anyway.

## Limits

| What | Limit | When it is reached |
| --- | --- | --- |
| Connections | `--max-connections` | The listening socket is left out of `poll()`. New clients wait in the kernel backlog. |
| Descriptors | `ulimit -n` | Each new connection is accepted and closed at once. |
| Frame length | `--max-frame` | ERROR 4, then Closing. |
| Input per connection | one frame of the largest size | The server stops reading from that connection. |
| Requests in flight per connection | 1 | Further requests wait as bytes in the input buffer. |
| Output per connection | `--output-high-water` | The server stops reading and decoding for that connection until the output drains to half. |
| COMPUTE iterations | 100,000 | ERROR 7, the connection stays open. |

The worker pool's task queue has no limit yet. It holds at most one
task per connection.
