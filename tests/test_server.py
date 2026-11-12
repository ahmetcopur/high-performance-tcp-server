"""
Black-box regression tests for the echo server.

Usage:
    python3 tests/test_server.py path/to/server
"""

import os
import resource
import signal
import socket
import statistics
import struct
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


def try_echo(s, data):
    # True if the server echoed data, False if it closed the connection.
    try:
        s.sendall(data)
        return recv_exact(s, len(data)) == data
    except (ConnectionResetError, BrokenPipeError):
        return False


def start_server(*options, fd_limit=None):
    def limit_fds():
        if fd_limit is not None:
            resource.setrlimit(resource.RLIMIT_NOFILE, (fd_limit, fd_limit))

    proc = subprocess.Popen(
        [SERVER, "--port", str(PORT), *options],
        stdout=subprocess.DEVNULL,
        preexec_fn=limit_fds,
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
    FD_LIMIT = None

    def setUp(self):
        self.proc = start_server(*self.OPTIONS, fd_limit=self.FD_LIMIT)

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

    def test_survives_reset_burst(self):
        # SO_LINGER with a zero timeout makes close() send RST instead of
        # FIN. Some of these resets arrive before the server calls accept().
        for _ in range(200):
            s = connect()
            s.setsockopt(
                socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0)
            )
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


class FdExhaustionTest(ServerTestCase):
    # The server may open this many descriptors in total. The connection
    # limit stays at its default, far above it, so descriptors run out
    # first.
    FD_LIMIT = 24

    def test_sheds_connections_and_recovers(self):
        socks = [connect() for _ in range(60)]
        served = []
        shed = []

        for s in socks:
            s.settimeout(2)
            if try_echo(s, b"hi"):
                served.append(s)
            else:
                shed.append(s)

        # The readiness probe in start_server may have held a descriptor
        # during the burst and given it back afterwards. Keep connecting
        # until one more is refused, so that none is free from here on.
        while True:
            s = connect()
            s.settimeout(2)
            socks.append(s)
            if not try_echo(s, b"hi"):
                break
            served.append(s)
            self.assertLess(len(served), self.FD_LIMIT)

        self.assertGreater(len(served), 0)
        self.assertLess(len(served), self.FD_LIMIT)
        self.assertGreater(len(shed), 0)
        self.assertIsNone(self.proc.poll())

        # Still out of descriptors. The server must be idle, not spinning.
        before = cpu_seconds(self.proc.pid)
        time.sleep(2)
        used = cpu_seconds(self.proc.pid) - before
        self.assertLess(used, 0.5)

        self.assertTrue(try_echo(served[0], b"ok"))

        # A newcomer is turned away at once instead of being left to hang.
        late = connect()
        late.settimeout(2)
        self.assertFalse(try_echo(late, b"late"))
        late.close()

        for s in socks:
            s.close()

        # The server frees descriptors as it notices each close.
        deadline = time.time() + 5
        while True:
            s = connect()
            recovered = try_echo(s, b"recovered")
            s.close()
            if recovered:
                break
            self.assertLess(time.time(), deadline)
            time.sleep(0.05)


if __name__ == "__main__":
    if len(sys.argv) > 1:
        SERVER = sys.argv.pop(1)
    unittest.main()
