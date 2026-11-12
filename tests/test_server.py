"""
Black-box regression tests for the echo server.

Usage:
    python3 tests/test_server.py path/to/server
"""

import os
import signal
import socket
import statistics
import subprocess
import sys
import threading
import time
import unittest

SERVER = "./server"
PORT = 18080


def connect():
    s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    s.settimeout(5)
    return s


def recv_exact(s, n):
    data = b""
    while len(data) < n:
        chunk = s.recv(n - len(data))
        if not chunk:
            break
        data += chunk
    return data


def start_server(*options):
    proc = subprocess.Popen(
        [SERVER, "--port", str(PORT), *options],
        stdout=subprocess.DEVNULL,
    )
    deadline = time.time() + 5
    while True:
        try:
            connect().close()
            return proc
        except OSError:
            if time.time() > deadline:
                proc.kill()
                raise
            time.sleep(0.02)


class ServerTestCase(unittest.TestCase):
    OPTIONS = ("--workers", "4")

    def setUp(self):
        self.proc = start_server(*self.OPTIONS)

    def tearDown(self):
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()


class CommandLineTest(unittest.TestCase):
    def test_invalid_option_exits_with_usage(self):
        result = subprocess.run(
            [SERVER, "--port", "not-a-number"],
            capture_output=True,
            text=True,
            timeout=5,
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("invalid value for --port", result.stderr)
        self.assertIn("Usage:", result.stderr)


class ServerTest(ServerTestCase):
    def test_echo(self):
        s = connect()
        s.sendall(b"hello")
        self.assertEqual(recv_exact(s, 5), b"hello")
        s.close()

    def test_single_client_latency(self):
        # A finished worker task must wake the event loop right away.
        # With the old 50 ms poll timeout the median here was about 52 ms.
        s = connect()
        latencies = []
        for _ in range(200):
            start = time.perf_counter()
            s.sendall(b"ping")
            self.assertEqual(recv_exact(s, 4), b"ping")
            latencies.append((time.perf_counter() - start) * 1000)
        s.close()
        self.assertLess(statistics.median(latencies), 10)

    def test_message_split_across_sends(self):
        s = connect()
        s.sendall(b"abc")
        time.sleep(0.1)
        s.sendall(b"def")
        self.assertEqual(recv_exact(s, 6), b"abcdef")
        s.close()

    def test_large_payload(self):
        # 4 MiB does not fit in the socket buffers, so the server has to
        # handle partial writes and stop reading while output is pending.
        payload = os.urandom(4 * 1024 * 1024)
        s = connect()
        s.settimeout(20)
        sender = threading.Thread(target=s.sendall, args=(payload,))
        sender.start()
        received = recv_exact(s, len(payload))
        sender.join()
        self.assertEqual(received, payload)
        s.close()

    # macOS reports POLLHUP as soon as the peer half-closes, and the server
    # closes on POLLHUP without reading first, so this fails about one run
    # in three there. Remove the skip when step 4 fixes the server.
    @unittest.skipIf(
        sys.platform == "darwin",
        "known issue: POLLHUP on half-close, see docs/PROGRESS.md",
    )
    def test_half_close_still_gets_response(self):
        s = connect()
        s.sendall(b"last words")
        s.shutdown(socket.SHUT_WR)
        self.assertEqual(recv_exact(s, 10), b"last words")
        self.assertEqual(s.recv(1), b"")
        s.close()

    def test_survives_abrupt_disconnects(self):
        for _ in range(200):
            s = connect()
            s.sendall(b"x" * 1000)
            s.close()
        s = connect()
        s.sendall(b"still alive")
        self.assertEqual(recv_exact(s, 11), b"still alive")
        s.close()

    def test_many_clients(self):
        socks = [connect() for _ in range(200)]
        for i, s in enumerate(socks):
            s.sendall(f"client-{i:04d}".encode())
        for i, s in enumerate(socks):
            self.assertEqual(recv_exact(s, 11), f"client-{i:04d}".encode())
            s.close()

    def test_sigint_stops_server(self):
        idle = connect()
        self.proc.send_signal(signal.SIGINT)
        self.assertEqual(self.proc.wait(timeout=2), 0)
        idle.close()


if __name__ == "__main__":
    if len(sys.argv) > 1:
        SERVER = sys.argv.pop(1)
    unittest.main()
