
import asyncio
import sys
import time
import statistics
import struct
import math

HOST = "127.0.0.1"
PORT = 8080
REQUESTS_PER_CLIENT = 200
MESSAGE_SIZE = 6

# Frame header from docs/protocol.md: length, version, type, reserved, id.
HEADER = struct.Struct(">IBBHI")
ECHO = 2
RESPONSE = 0x80


async def simulate_client(client_id):
    reader, writer = await asyncio.wait_for(
        asyncio.open_connection(HOST, PORT),
        timeout=15
    )

    latencies = []

    try:
        for i in range(REQUESTS_PER_CLIENT):
            payload = f"{client_id}:{i}:".encode()
            payload = payload.ljust(MESSAGE_SIZE, b"x")

            length = 8 + len(payload)
            message = HEADER.pack(length, 1, ECHO, 0, i) + payload
            expected = HEADER.pack(length, 1, ECHO | RESPONSE, 0, i) + payload

            start = time.perf_counter()

            writer.write(message)
            await asyncio.wait_for(writer.drain(), timeout=10)

            response = await asyncio.wait_for(
                reader.readexactly(len(message)),
                timeout=10
            )

            latency = (time.perf_counter() - start) * 1000

            if response != expected:
                raise ValueError("Incorrect echo response")

            latencies.append(latency)

    finally:
        writer.close()
        try:
            await asyncio.wait_for(writer.wait_closed(), timeout=2)
        except (OSError, asyncio.TimeoutError):
            pass

    return latencies


def percentile(values, p):
    index = math.ceil(len(values) * p / 100) - 1
    return values[max(0, index)]


async def benchmark(num_clients):
    print(f"\nRunning benchmark: {num_clients} clients")

    start = time.perf_counter()

    results = await asyncio.gather(
        *(simulate_client(i) for i in range(num_clients)),
        return_exceptions=True
    )

    elapsed = time.perf_counter() - start

    latencies = []
    errors = []

    for result in results:
        if isinstance(result, BaseException):
            errors.append(result)
        else:
            latencies.extend(result)

    latencies.sort()

    print(f"Elapsed: {elapsed:.2f} seconds")
    print(f"Successful clients: {num_clients - len(errors)}")
    print(f"Failed clients: {len(errors)}")
    print(f"Completed requests: {len(latencies)}")

    if not latencies:
        if errors:
            print(f"First error: {errors[0]}")
        return

    throughput = len(latencies) / elapsed
    bandwidth = len(latencies) * MESSAGE_SIZE / elapsed / 1048576

    print(f"Throughput: {throughput:.2f} requests/sec")
    print(f"Echo bandwidth: {bandwidth:.3f} MiB/sec")
    print(f"Average latency: {statistics.mean(latencies):.2f} ms")
    print(f"Median latency: {statistics.median(latencies):.2f} ms")
    print(f"P95 latency: {percentile(latencies, 95):.2f} ms")
    print(f"P99 latency: {percentile(latencies, 99):.2f} ms")

    if errors:
        print(f"First error: {errors[0]}")


if __name__ == "__main__":
    clients = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    asyncio.run(benchmark(clients))
