# Wire protocol, version 1

A client sends request frames over one TCP connection and the server
answers each with one response frame. All integers are unsigned and
big-endian (most significant byte first).

## Frame layout

Requests and responses share one layout.

```text
Offset  Size  Field
0       4     length      Number of bytes after this field. At least 8.
4       1     version     1
5       1     type        See "Message types".
6       2     reserved    0
8       4     request_id  Chosen by the client, copied into the response.
12      n     payload     n = length - 8
```

A frame occupies `4 + length` bytes on the wire. The smallest frame has
no payload: `length` is 8 and the frame is 12 bytes.

`length` may not exceed the server's frame limit, set with `--max-frame`
(default 1,048,576). The largest payload is therefore the limit minus 8.

TCP delivers a byte stream, not messages. A frame may arrive in several
pieces, and several frames may arrive in one piece. The `length` field is
the only thing that says where a frame ends.

## Message types

| Type | Value | Direction | Payload |
| --- | --- | --- | --- |
| PING | `0x01` | request | Ignored. |
| PING response | `0x81` | response | Empty. |
| ECHO | `0x02` | request | Any bytes. |
| ECHO response | `0x82` | response | The request payload, unchanged. |
| COMPUTE | `0x03` | request | Iteration count (4 bytes), then data. |
| COMPUTE response | `0x83` | response | Hash (8 bytes). |
| STATS | `0x04` | request | Ignored. |
| STATS response | `0x84` | response | Ten counters, 8 bytes each. |
| ERROR | `0xFF` | response | Error code (2 bytes), then a message. |

A response has the type of its request with the high bit set
(`0x80 | type`) and the same `request_id`.

### COMPUTE

The request payload is a 4-byte iteration count followed by the data to
hash. The data may be empty. The response payload is the 8-byte result
of 64-bit FNV-1a run over the data `iterations` times, without resetting
the hash between passes:

```python
def compute(data: bytes, iterations: int) -> int:
    h = 0xcbf29ce484222325
    for _ in range(iterations):
        for byte in data:
            h = ((h ^ byte) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h
```

The iteration count may not exceed 100,000. A larger count, or a payload
shorter than 4 bytes, is answered with ERROR 7.

### STATS

The response payload is ten 64-bit integers in this order, 80 bytes:

| Offset | Field | Meaning |
| --- | --- | --- |
| 0 | connections | Connections open right now. |
| 8 | queue_depth | Requests waiting for a worker. Requests being worked on are not counted. |
| 16 | queue_capacity | How many requests may wait. |
| 24 | workers | Worker threads. |
| 32 | tasks_submitted | Requests handed to workers since the server started. |
| 40 | tasks_rejected | Requests answered with ERROR 5 because the queue was full. |
| 48 | tasks_completed | Requests workers have finished. |
| 56 | tasks_failed | Of those, the ones answered with ERROR 6. |
| 64 | worker_busy_us | Microseconds workers have spent on requests, summed over all workers. |
| 72 | uptime_us | Microseconds since the server started. |

Fields may be added at the end in a later revision. A client must accept
a payload longer than 80 bytes and ignore what it does not know.

The counters that workers keep (`tasks_completed`, `tasks_failed`,
`worker_busy_us`) can trail a response by a moment. A worker hands its
result to the event loop first and is counted as done just after, so a
STATS sent right behind a request may not include that request yet.
`tasks_submitted` and `tasks_rejected` are exact.

PING and STATS are answered without a worker, so they are still served
while every worker is busy, and they are not counted in the task
counters. The share of time the workers were busy between two STATS
responses is the difference in `worker_busy_us`, divided by the
difference in `uptime_us` times `workers`.

## Ordering

Responses on one connection come back in the order the requests were
sent. A client may send several requests without waiting (pipelining)
and match the responses by position or by `request_id`.

The server does not check `request_id` for uniqueness.

## Errors

An ERROR frame's payload is a 2-byte error code followed by a UTF-8
message. The message is for people. Programs should look at the code.

| Code | Name | Meaning | Connection |
| --- | --- | --- | --- |
| 1 | malformed frame | `length` below 8 | closed |
| 1 | malformed frame | `reserved` is not 0 | stays open |
| 2 | unsupported version | `version` is not 1 | stays open |
| 3 | unknown type | `type` is not a request the server knows | stays open |
| 4 | payload too large | `length` above the frame limit | closed |
| 5 | server busy | The queue for the workers is full | stays open |
| 6 | internal error | The server failed while handling the request | stays open |
| 7 | bad request | The payload is wrong for its type | stays open |

There are two kinds of error, and the difference is whether the server
still knows where the next frame starts.

**The length cannot be used** (below 8, or above the limit). Every later
byte would be guessed at, so the stream is lost. The server sends an
ERROR with `request_id` 0, because it never trusted the frame enough to
read one, then stops reading, sends what it still owes, and closes the
connection. Responses to earlier requests are sent first.

**The length is fine but something else is wrong** (version, reserved,
type, or a payload the request type does not accept). The frame boundary
is known, so the server skips the frame, answers it with an ERROR that
carries its `request_id`, and continues with the next frame.

The version is checked first. When it is wrong, the other fields are not
examined, because they may mean something else in that version.

### Server busy

Requests that need a worker wait in a queue of limited size
(`--queue-capacity`, default 1024). A request that finds the queue full
is not queued and not dropped. It is answered at once with ERROR 5 and
its own `request_id`, in its place in the order of responses. The
connection stays open, and the next request may find room. A client
that gets ERROR 5 should wait a little before sending that request
again.

## Worked examples

Bytes are shown in hex.

**ECHO** with `request_id` 7 and payload `hi`:

```text
request   00 00 00 0a  01  02  00 00  00 00 00 07  68 69
          length=10    v1  ECHO  rsv  id=7         "hi"

response  00 00 00 0a  01  82  00 00  00 00 00 07  68 69
```

**PING** with `request_id` 1:

```text
request   00 00 00 08  01  01  00 00  00 00 00 01
response  00 00 00 08  01  81  00 00  00 00 00 01
```

**COMPUTE** with `request_id` 8, 3 iterations over `abc`:

```text
request   00 00 00 0f  01  03  00 00  00 00 00 08  00 00 00 03  61 62 63
          length=15    v1  COMPUTE    id=8         iterations   "abc"

response  00 00 00 10  01  83  00 00  00 00 00 08  d3 28 b5 61 82 a2 5b 1b
          length=16                                hash 0xd328b56182a25b1b
```

**ERROR** for a request with `request_id` 9 and the unknown type `0x09`:

```text
request   00 00 00 08  01  09  00 00  00 00 00 09

response  00 00 00 16  01  ff  00 00  00 00 00 09  00 03  75 6e 6b 6e 6f 77 6e 20 74 79 70 65
          length=22        ERROR      id=9         code=3 "unknown type"
```

**STATS** with `request_id` 5, on a server with 8 workers that has
been up for one minute:

```text
request   00 00 00 08  01  04  00 00  00 00 00 05

response  00 00 00 58  01  84  00 00  00 00 00 05
          length=88        STATS      id=5

          00 00 00 00 00 00 00 03    connections        3
          00 00 00 00 00 00 00 00    queue_depth        0
          00 00 00 00 00 00 04 00    queue_capacity     1024
          00 00 00 00 00 00 00 08    workers            8
          00 00 00 00 00 00 00 78    tasks_submitted    120
          00 00 00 00 00 00 00 00    tasks_rejected     0
          00 00 00 00 00 00 00 78    tasks_completed    120
          00 00 00 00 00 00 00 00    tasks_failed       0
          00 00 00 00 00 00 af c8    worker_busy_us     45000
          00 00 00 00 03 93 87 00    uptime_us          60000000
```

**ERROR 5** for an ECHO with `request_id` 7 that found the queue full:

```text
response  00 00 00 15  01  ff  00 00  00 00 00 07  00 05  73 65 72 76 65 72 20 62 75 73 79
          length=21        ERROR      id=7         code=5 "server busy"
```

## Not in version 1

- No authentication, encryption, or checksum.
- No way for the server to send anything except responses.
- No negotiation. A client that speaks another version gets ERROR 2.
