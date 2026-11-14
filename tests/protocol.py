"""
Client side of the wire protocol described in docs/protocol.md.

Written independently of the C++ codec, straight from the document, so
that the integration tests check the server against the specification
and not against itself.
"""

import struct

VERSION = 1

PING = 1
ECHO = 2
COMPUTE = 3
STATS = 4
ERROR = 0xFF
RESPONSE = 0x80

ERR_MALFORMED_FRAME = 1
ERR_UNSUPPORTED_VERSION = 2
ERR_UNKNOWN_TYPE = 3
ERR_PAYLOAD_TOO_LARGE = 4
ERR_SERVER_BUSY = 5
ERR_INTERNAL = 6
ERR_BAD_REQUEST = 7

DEFAULT_MAX_FRAME = 1 << 20
MAX_COMPUTE_ITERATIONS = 100000

# length, version, type, reserved, request id
HEADER = struct.Struct(">IBBHI")


def encode(msg_type, request_id, payload=b"", version=VERSION, reserved=0):
    return (
        HEADER.pack(8 + len(payload), version, msg_type, reserved, request_id)
        + payload
    )


def recv_exact(sock, n):
    # Returns fewer than n bytes only if the peer closed the connection.
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            break
        data += chunk
    return data


def recv_frame(sock):
    # Returns (type, request_id, payload), or None if the server closed
    # the connection cleanly between frames.
    header = recv_exact(sock, HEADER.size)
    if not header:
        return None
    if len(header) < HEADER.size:
        raise EOFError("connection closed inside a frame header")

    length, version, msg_type, reserved, request_id = HEADER.unpack(header)
    if version != VERSION or reserved != 0 or length < 8:
        raise ValueError(f"server sent a bad header: {header.hex()}")

    payload = recv_exact(sock, length - 8)
    if len(payload) < length - 8:
        raise EOFError("connection closed inside a frame payload")

    return msg_type, request_id, payload


def request(sock, msg_type, request_id, payload=b""):
    sock.sendall(encode(msg_type, request_id, payload))
    return recv_frame(sock)


def compute_payload(iterations, data):
    return struct.pack(">I", iterations) + data


def compute_reference(data, iterations):
    # 64-bit FNV-1a, run over data `iterations` times without a reset.
    h = 0xCBF29CE484222325
    for _ in range(iterations):
        for byte in data:
            h = ((h ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def compute_response(request_id, data, iterations):
    # The frame a correct server sends back for a COMPUTE request.
    return (
        COMPUTE | RESPONSE,
        request_id,
        struct.pack(">Q", compute_reference(data, iterations)),
    )


def error_code(frame):
    # The code of an ERROR frame returned by recv_frame().
    msg_type, _, payload = frame
    if msg_type != ERROR or len(payload) < 2:
        raise ValueError(f"not an ERROR frame: {frame!r}")
    return struct.unpack(">H", payload[:2])[0]


# The fields of a STATS response, in wire order. Each is a 64-bit
# big-endian integer.
STATS_FIELDS = (
    "connections",
    "queue_depth",
    "queue_capacity",
    "workers",
    "tasks_submitted",
    "tasks_rejected",
    "tasks_completed",
    "tasks_failed",
    "worker_busy_us",
    "uptime_us",
)


def parse_stats(frame):
    # The counters in a STATS response returned by recv_frame(), by name.
    # A longer payload is accepted, since fields may be added at the end.
    msg_type, _, payload = frame
    size = 8 * len(STATS_FIELDS)
    if msg_type != (STATS | RESPONSE) or len(payload) < size:
        raise ValueError(f"not a STATS response: {frame!r}")
    values = struct.unpack(f">{len(STATS_FIELDS)}Q", payload[:size])
    return dict(zip(STATS_FIELDS, values))
