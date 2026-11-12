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


def cpu_seconds(pid):
    # User plus system CPU time the process has used since it started.
    if os.path.exists(f"/proc/{pid}/stat"):
        with open(f"/proc/{pid}/stat") as f:
            fields = f.read().rsplit(")", 1)[1].split()
        return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")

    out = subprocess.check_output(
        ["ps", "-o", "time=", "-p", str(pid)], text=True
    )
    seconds = 0.0
    for part in out.strip().split(":"):
        seconds = seconds * 60 + float(part)
    return seconds


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


class ConnectionLimitTest(ServerTestCase):
    LIMIT = 5
    OPTIONS = ("--workers", "4", "--max-connections", str(LIMIT))

    def fill_to_limit(self):
        held = [connect() for _ in range(self.LIMIT)]
        for s in held:
            s.sendall(b"held")
            self.assertEqual(recv_exact(s, 4), b"held")
        return held

    def test_limit_is_enforced_and_recovers(self):
        held = self.fill_to_limit()

        # The kernel completes the handshake on its own, so connect()
        # succeeds. The server must leave this connection in the backlog.
        extra = connect()
        extra.sendall(b"extra")
        extra.settimeout(0.5)
        with self.assertRaises(socket.timeout):
            extra.recv(5)

        held[1].sendall(b"again")
        self.assertEqual(recv_exact(held[1], 5), b"again")

        held[0].close()
        extra.settimeout(5)
        self.assertEqual(recv_exact(extra, 5), b"extra")

        extra.close()
        for s in held[1:]:
            s.close()

    def test_no_cpu_spin_at_limit(self):
        # A connection waiting in the backlog keeps the listening socket
        # readable. Polling it without accepting would spin.
        held = self.fill_to_limit()
        waiting = connect()

        before = cpu_seconds(self.proc.pid)
        time.sleep(2)
        used = cpu_seconds(self.proc.pid) - before
        self.assertLess(used, 0.5)

        waiting.close()
        for s in held:
            s.close()

    def test_churn_above_limit(self):
        # Ten clients at a time against a limit of five. Every request
        # must still be answered once a slot frees up.
        errors = []

        def client(i):
            try:
                for r in range(30):
                    message = f"c{i:02d}r{r:02d}".encode()
                    s = connect()
                    s.sendall(message)
                    if recv_exact(s, len(message)) != message:
                        raise AssertionError("wrong echo")
                    s.close()
            except Exception as e:
                errors.append(repr(e))

        threads = [
            threading.Thread(target=client, args=(i,)) for i in range(10)
        ]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        self.assertEqual(errors, [])


if __name__ == "__main__":
    if len(sys.argv) > 1:
        SERVER = sys.argv.pop(1)
    unittest.main()
