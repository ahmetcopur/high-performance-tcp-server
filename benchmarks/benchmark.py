#!/usr/bin/env python3
"""
A quick smoke test, not a benchmark. It checks that a running server
answers ECHO correctly under some concurrent load and prints rough
numbers.

    python3 benchmarks/benchmark.py [clients] [--port N] [--requests N] [--payload N]

For numbers worth quoting use benchmarks/run.py, which drives the C++
load generator. This script is a Python asyncio client on one thread,
and it is usually the client, not the server, that limits what it
reports.
"""

import argparse
import asyncio
import math
import statistics
import struct
import time

HOST = "127.0.0.1"

# Frame header from docs/protocol.md: length, version, type, reserved, id.
HEADER = struct.Struct(">IBBHI")
ECHO = 2
RESPONSE = 0x80


async def connect(port):
    return await asyncio.wait_for(asyncio.open_connection(HOST, port), timeout=15)


async def run_client(reader, writer, client_id, requests, payload_size):
    # Returns (latencies in ms, bytes sent, bytes received).
    latencies = []
    sent = received = 0

    try:
        for i in range(requests):
            # Exactly payload_size bytes: the label is cut or padded.
            payload = f"{client_id}:{i}:".encode()[:payload_size]
            payload = payload.ljust(payload_size, b"x")

            length = 8 + len(payload)
            message = HEADER.pack(length, 1, ECHO, 0, i) + payload
            expected = HEADER.pack(length, 1, ECHO | RESPONSE, 0, i) + payload

            start = time.perf_counter()

            writer.write(message)
            await asyncio.wait_for(writer.drain(), timeout=10)
            response = await asyncio.wait_for(
                reader.readexactly(len(expected)), timeout=10
            )

            latencies.append((time.perf_counter() - start) * 1000)
            sent += len(message)
            received += len(response)

            if response != expected:
                raise ValueError("wrong answer to ECHO")
    finally:
        writer.close()
        try:
            await asyncio.wait_for(writer.wait_closed(), timeout=2)
        except (OSError, asyncio.TimeoutError):
            pass

    return latencies, sent, received


def percentile(sorted_values, p):
    index = math.ceil(len(sorted_values) * p / 100) - 1
    return sorted_values[max(0, index)]


async def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("clients", nargs="?", type=int, default=10)
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--requests", type=int, default=200, help="per client")
    parser.add_argument("--payload", type=int, default=64, help="bytes")
    args = parser.parse_args()

    # Connect first, a hundred at a time, so that the server's backlog is
    # not overrun and connection setup is not part of what is timed.
    connections = []
    for first in range(0, args.clients, 100):
        batch = range(first, min(first + 100, args.clients))
        connections += await asyncio.gather(*(connect(args.port) for _ in batch))

    start = time.perf_counter()

    results = await asyncio.gather(
        *(
            run_client(reader, writer, i, args.requests, args.payload)
            for i, (reader, writer) in enumerate(connections)
        ),
        return_exceptions=True,
    )

    elapsed = time.perf_counter() - start

    failures = [r for r in results if isinstance(r, BaseException)]
    latencies = sorted(l for r in results if not isinstance(r, BaseException) for l in r[0])
    sent = sum(r[1] for r in results if not isinstance(r, BaseException))
    received = sum(r[2] for r in results if not isinstance(r, BaseException))

    print(f"{args.clients} clients, {args.requests} requests each, {args.payload} byte payload")
    print(f"clients that failed: {len(failures)}")

    if failures:
        print(f"first failure: {failures[0]!r}")

    if not latencies:
        raise SystemExit(1)

    print(f"requests answered correctly: {len(latencies)} in {elapsed:.2f} s")
    print(f"throughput: {len(latencies) / elapsed:,.0f} requests/s")
    print(
        f"bytes on the wire: {sent / elapsed / 1048576:.2f} MiB/s sent, "
        f"{received / elapsed / 1048576:.2f} MiB/s received"
    )
    print(
        f"latency: median {statistics.median(latencies):.2f} ms, "
        f"p95 {percentile(latencies, 95):.2f} ms, p99 {percentile(latencies, 99):.2f} ms"
    )
    print(
        "Each client waits for an answer before it sends again, so these\n"
        "latencies are mostly the number of clients divided by the throughput."
    )

    raise SystemExit(1 if failures else 0)


if __name__ == "__main__":
    asyncio.run(main())
