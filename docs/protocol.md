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
| STATS | `0x04` | request | Reserved. Not implemented, answered with ERROR 3. |
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
| 5 | server busy | Reserved for overload. Not sent yet. | stays open |
| 6 | internal error | Reserved. Not sent yet. | stays open |
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

## Not in version 1

- No authentication, encryption, or checksum.
- No way for the server to send anything except responses.
- No negotiation. A client that speaks another version gets ERROR 2.
