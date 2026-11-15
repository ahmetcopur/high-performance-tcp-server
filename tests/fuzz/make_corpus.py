#!/usr/bin/env python3
"""
Writes the seed corpus for the fuzz targets: a few inputs that already
reach the interesting parts, for the fuzzer to start from.

    python3 tests/fuzz/make_corpus.py

The files are committed. This script is how they were made, and how to
make them again after the protocol changes. Inputs the fuzzer found by
itself are added to the same directories with its -merge=1 option.
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import protocol  # noqa: E402
from protocol import COMPUTE, ECHO, ERROR, PING, STATS  # noqa: E402

frames = {
    "ping": protocol.encode(PING, 1),
    "echo-empty": protocol.encode(ECHO, 2),
    "echo": protocol.encode(ECHO, 3, b"hello, server"),
    "echo-binary": protocol.encode(ECHO, 4, bytes(range(256))),
    "compute": protocol.encode(COMPUTE, 5, protocol.compute_payload(3, b"abc")),
    "compute-no-data": protocol.encode(COMPUTE, 6, protocol.compute_payload(1, b"")),
    "compute-short": protocol.encode(COMPUTE, 7, b"\x00\x01"),
    "compute-too-many": protocol.encode(COMPUTE, 8, protocol.compute_payload(100001, b"x")),
    "stats": protocol.encode(STATS, 9),
    "stats-payload": protocol.encode(STATS, 10, bytes(80)),
    "error": protocol.encode(ERROR, 11, struct.pack(">H", 5) + b"server busy"),
    "response": protocol.encode(ECHO | 0x80, 12, b"a response sent as a request"),
    "bad-version": protocol.encode(ECHO, 13, b"x", version=2),
    "bad-reserved": protocol.encode(ECHO, 14, b"x", reserved=1),
    "unknown-type": protocol.encode(0x7F, 15, b"x"),
    "length-zero": struct.pack(">I", 0),
    "length-seven": struct.pack(">I", 7) + bytes(7),
    "length-huge": struct.pack(">I", 0xFFFFFFFF),
    "length-just-over": struct.pack(">I", 1025) + bytes(16),
    "header-cut": protocol.encode(ECHO, 16, b"cut")[:9],
    "payload-cut": protocol.encode(ECHO, 17, b"cut off in the payload")[:20],
    "three-bytes": b"\x00\x00\x00",
    "empty": b"",
}

streams = {
    "one": frames["echo"],
    "pipeline": b"".join(
        protocol.encode(kind, i, b"p%d" % i)
        for i, kind in enumerate([ECHO, PING, ECHO, STATS, ECHO, PING])
    ),
    "errors-between": frames["echo"] + frames["unknown-type"] + frames["ping"]
    + frames["bad-version"] + frames["echo-empty"] + frames["bad-reserved"],
    "bad-length-last": frames["echo"] + frames["ping"] + frames["length-zero"],
    "bytes-after-bad-length": frames["ping"] + frames["length-huge"] + frames["echo"],
    "ends-inside-a-frame": frames["echo"] + frames["payload-cut"],
    "largest": protocol.encode(ECHO, 20, bytes(248)),
    "too-large": protocol.encode(ECHO, 21, bytes(249)),
    "nothing": b"",
}


def write(directory, name, data):
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, name), "wb") as f:
        f.write(data)


for name, data in frames.items():
    write(os.path.join(HERE, "corpus", "decode"), name, data)

# The first byte of a stream input says how the rest is cut up, and
# whether answers come a turn late (the top bit).
for name, data in streams.items():
    for label, first in (("whole", 0x00), ("by1", 0x01), ("by7", 0x07),
                         ("by1-late", 0x81), ("by13-late", 0x8D), ("whole-late", 0x80)):
        write(os.path.join(HERE, "corpus", "stream"), f"{name}-{label}",
              bytes([first]) + data)
