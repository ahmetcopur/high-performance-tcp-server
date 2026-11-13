"""
Black-box tests for the server. They start the real binary and talk to
it over TCP with the protocol in docs/protocol.md.

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

import protocol
from protocol import COMPUTE, ECHO, ERROR, PING, RESPONSE, STATS

SERVER = "./server"
PORT = 18080


def connect():
    s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    s.settimeout(5)
    return s


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


def rss_bytes(pid):
    # Resident memory of the process. ps reports it in KiB.
    out = subprocess.check_output(
        ["ps", "-o", "rss=", "-p", str(pid)], text=True
    )
    return int(out.strip()) * 1024


def count_fds(pid):
    # Number of descriptors the process has open.
    if os.path.isdir(f"/proc/{pid}/fd"):
        return len(os.listdir(f"/proc/{pid}/fd"))

    out = subprocess.run(
        ["lsof", "-p", str(pid), "-Ff"], capture_output=True, text=True
    ).stdout
    return sum(
        1 for line in out.splitlines() if line[:1] == "f" and line[1:].isdigit()
    )


def settled_fd_count(pid):
    # The count once it has stopped changing, so that connections the
    # server is still closing are not counted.
    previous = count_fds(pid)
    while True:
        time.sleep(0.2)
        current = count_fds(pid)
        if current == previous:
            return current
        previous = current


def wait_for_fd_count(pid, expected, timeout=5):
    # Returns the count as soon as it equals expected, or the last count
    # seen when the time is up.
    deadline = time.time() + timeout
    while True:
        count = count_fds(pid)
        if count == expected or time.time() > deadline:
            return count
        time.sleep(0.05)


def wait_closed(s, timeout):
    # Seconds until the server ends the connection, reading and dropping
    # anything it sends first. None if it is still open after timeout.
    # A reset counts as closed.
    start = time.time()
    s.settimeout(timeout)
    try:
        while s.recv(65536):
            pass
    except (ConnectionResetError, BrokenPipeError):
        pass
    except socket.timeout:
        return None
    return time.time() - start


def reset(s):
    # SO_LINGER with a zero timeout makes close() send RST instead of FIN.
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    s.close()


def try_echo(s, data, request_id=1):
    # True if the server echoed data, False if it closed the connection.
    try:
        reply = protocol.request(s, ECHO, request_id, data)
        return reply == (ECHO | RESPONSE, request_id, data)
    except (ConnectionResetError, BrokenPipeError, EOFError):
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

    def assert_echo(self, s, data, request_id=1):
        self.assertEqual(
            protocol.request(s, ECHO, request_id, data),
            (ECHO | RESPONSE, request_id, data),
        )

    def assert_error(self, frame, code, request_id):
        self.assertEqual(frame[0], ERROR)
        self.assertEqual(frame[1], request_id)
        self.assertEqual(protocol.error_code(frame), code)


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
        self.assert_echo(s, b"hello", request_id=7)
        self.assert_echo(s, b"", request_id=8)
        self.assert_echo(s, bytes(range(256)), request_id=0xFFFFFFFF)
        s.close()

    def test_ping(self):
        s = connect()
        self.assertEqual(
            protocol.request(s, PING, 5), (PING | RESPONSE, 5, b"")
        )
        # A payload on PING is ignored, not echoed.
        self.assertEqual(
            protocol.request(s, PING, 6, b"ignored"), (PING | RESPONSE, 6, b"")
        )
        s.close()

    def test_single_client_latency(self):
        # A finished worker task must wake the event loop right away.
        # With the old 50 ms poll timeout the median here was about 52 ms.
        s = connect()
        latencies = []
        for i in range(200):
            start = time.perf_counter()
            self.assert_echo(s, b"ping", request_id=i)
            latencies.append((time.perf_counter() - start) * 1000)
        s.close()
        self.assertLess(statistics.median(latencies), 10)

    def test_frame_split_across_sends(self):
        frame = protocol.encode(ECHO, 3, b"abcdef")
        s = connect()
        s.sendall(frame[:7])
        time.sleep(0.1)
        s.sendall(frame[7:])
        self.assertEqual(protocol.recv_frame(s), (ECHO | RESPONSE, 3, b"abcdef"))
        s.close()

    def test_frame_one_byte_at_a_time(self):
        frame = protocol.encode(ECHO, 4, b"slow")
        s = connect()
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        for i in range(len(frame)):
            s.sendall(frame[i : i + 1])
            time.sleep(0.005)
        self.assertEqual(protocol.recv_frame(s), (ECHO | RESPONSE, 4, b"slow"))
        s.close()

    def test_frames_merged_in_one_send(self):
        # Two whole frames and the first half of a third in one write.
        third = protocol.encode(ECHO, 3, b"third")
        s = connect()
        s.sendall(
            protocol.encode(ECHO, 1, b"first")
            + protocol.encode(PING, 2)
            + third[:9]
        )
        self.assertEqual(protocol.recv_frame(s), (ECHO | RESPONSE, 1, b"first"))
        self.assertEqual(protocol.recv_frame(s), (PING | RESPONSE, 2, b""))
        s.sendall(third[9:])
        self.assertEqual(protocol.recv_frame(s), (ECHO | RESPONSE, 3, b"third"))
        s.close()

    def test_pipelined_requests_come_back_in_order(self):
        # 300 requests of all three types in a single write. PING is
        # answered on the event loop and the others by workers, so this
        # fails if a quick answer overtakes a slow one.
        requests = b""
        expected = []
        for i in range(300):
            if i % 3 == 0:
                data = bytes([i % 256]) * (i * 7 % 500)
                requests += protocol.encode(ECHO, i, data)
                expected.append((ECHO | RESPONSE, i, data))
            elif i % 3 == 1:
                requests += protocol.encode(PING, i)
                expected.append((PING | RESPONSE, i, b""))
            else:
                data = b"compute-%d" % i
                requests += protocol.encode(
                    COMPUTE, i, protocol.compute_payload(i % 20, data)
                )
                expected.append(protocol.compute_response(i, data, i % 20))

        s = connect()
        s.sendall(requests)
        for frame in expected:
            self.assertEqual(protocol.recv_frame(s), frame)
        s.close()

    def test_compute_matches_reference(self):
        cases = [
            (b"", 0),
            (b"", 5),
            (b"a", 1),
            (b"abc", 3),
            (b"hello world", 1000),
            (bytes(range(256)), 17),
            (b"x" * 5000, 2),
        ]
        s = connect()
        for i, (data, iterations) in enumerate(cases):
            reply = protocol.request(
                s, COMPUTE, i, protocol.compute_payload(iterations, data)
            )
            self.assertEqual(
                reply, protocol.compute_response(i, data, iterations)
            )
        s.close()

    def test_compute_at_the_iteration_limit(self):
        limit = protocol.MAX_COMPUTE_ITERATIONS
        s = connect()

        reply = protocol.request(
            s, COMPUTE, 1, protocol.compute_payload(limit, b"z")
        )
        self.assertEqual(reply, protocol.compute_response(1, b"z", limit))

        reply = protocol.request(
            s, COMPUTE, 2, protocol.compute_payload(limit + 1, b"z")
        )
        self.assert_error(reply, protocol.ERR_BAD_REQUEST, 2)

        self.assert_echo(s, b"still open")
        s.close()

    def test_compute_payload_too_short(self):
        s = connect()
        for i, payload in enumerate([b"", b"\x00", b"\x00\x00\x00"]):
            reply = protocol.request(s, COMPUTE, i, payload)
            self.assert_error(reply, protocol.ERR_BAD_REQUEST, i)
        self.assert_echo(s, b"still open")
        s.close()

    def test_unknown_type_returns_error_and_connection_survives(self):
        s = connect()
        for i, msg_type in enumerate([0, STATS, 9, 0x7F, ECHO | RESPONSE, ERROR]):
            reply = protocol.request(s, msg_type, i, b"whatever")
            self.assert_error(reply, protocol.ERR_UNKNOWN_TYPE, i)
        self.assert_echo(s, b"still open")
        s.close()

    def test_bad_version_returns_error_and_connection_survives(self):
        s = connect()
        s.sendall(protocol.encode(ECHO, 11, b"v2", version=2))
        self.assert_error(
            protocol.recv_frame(s), protocol.ERR_UNSUPPORTED_VERSION, 11
        )
        self.assert_echo(s, b"still open")
        s.close()

    def test_nonzero_reserved_returns_error_and_connection_survives(self):
        s = connect()
        s.sendall(protocol.encode(ECHO, 12, b"rsv", reserved=1))
        self.assert_error(
            protocol.recv_frame(s), protocol.ERR_MALFORMED_FRAME, 12
        )
        self.assert_echo(s, b"still open")
        s.close()

    def test_errors_keep_their_place_in_the_pipeline(self):
        s = connect()
        s.sendall(
            protocol.encode(ECHO, 1, b"before")
            + protocol.encode(9, 2)
            + protocol.encode(ECHO, 3, b"v", version=3)
            + protocol.encode(PING, 4)
            + protocol.encode(ECHO, 5, b"after")
        )
        self.assertEqual(protocol.recv_frame(s), (ECHO | RESPONSE, 1, b"before"))
        self.assert_error(protocol.recv_frame(s), protocol.ERR_UNKNOWN_TYPE, 2)
        self.assert_error(
            protocol.recv_frame(s), protocol.ERR_UNSUPPORTED_VERSION, 3
        )
        self.assertEqual(protocol.recv_frame(s), (PING | RESPONSE, 4, b""))
        self.assertEqual(protocol.recv_frame(s), (ECHO | RESPONSE, 5, b"after"))
        s.close()

    def test_length_below_minimum_closes_after_error(self):
        for length in (0, 7):
            s = connect()
            s.sendall(struct.pack(">I", length))
            self.assert_error(
                protocol.recv_frame(s), protocol.ERR_MALFORMED_FRAME, 0
            )
            self.assertIsNone(protocol.recv_frame(s))
            s.close()

    def test_oversize_length_closes_after_error(self):
        # Only the length field is sent. The server must refuse without
        # waiting for a megabyte that is never coming.
        for length in (protocol.DEFAULT_MAX_FRAME + 1, 0xFFFFFFFF):
            s = connect()
            s.sendall(struct.pack(">I", length))
            self.assert_error(
                protocol.recv_frame(s), protocol.ERR_PAYLOAD_TOO_LARGE, 0
            )
            self.assertIsNone(protocol.recv_frame(s))
            s.close()

    def test_bad_length_is_reported_after_earlier_responses(self):
        s = connect()
        s.sendall(
            protocol.encode(ECHO, 1, b"good")
            + protocol.encode(COMPUTE, 2, protocol.compute_payload(50, b"data"))
            + struct.pack(">I", 3)
        )
        self.assertEqual(protocol.recv_frame(s), (ECHO | RESPONSE, 1, b"good"))
        self.assertEqual(
            protocol.recv_frame(s), protocol.compute_response(2, b"data", 50)
        )
        self.assert_error(
            protocol.recv_frame(s), protocol.ERR_MALFORMED_FRAME, 0
        )
        self.assertIsNone(protocol.recv_frame(s))
        s.close()

    def test_largest_frame_is_accepted(self):
        payload = os.urandom(protocol.DEFAULT_MAX_FRAME - 8)
        s = connect()
        s.settimeout(20)
        sender = threading.Thread(
            target=s.sendall, args=(protocol.encode(ECHO, 1, payload),)
        )
        sender.start()
        reply = protocol.recv_frame(s)
        sender.join()
        self.assertEqual(reply, (ECHO | RESPONSE, 1, payload))
        s.close()

    def test_half_close_still_gets_response(self):
        s = connect()
        s.sendall(protocol.encode(ECHO, 1, b"last words"))
        s.shutdown(socket.SHUT_WR)
        self.assertEqual(
            protocol.recv_frame(s), (ECHO | RESPONSE, 1, b"last words")
        )
        self.assertIsNone(protocol.recv_frame(s))
        s.close()

    def test_half_close_after_pipelined_requests(self):
        s = connect()
        s.sendall(
            protocol.encode(ECHO, 1, b"one")
            + protocol.encode(COMPUTE, 2, protocol.compute_payload(10, b"two"))
            + protocol.encode(ECHO, 3, b"three")
        )
        s.shutdown(socket.SHUT_WR)
        self.assertEqual(protocol.recv_frame(s), (ECHO | RESPONSE, 1, b"one"))
        self.assertEqual(
            protocol.recv_frame(s), protocol.compute_response(2, b"two", 10)
        )
        self.assertEqual(protocol.recv_frame(s), (ECHO | RESPONSE, 3, b"three"))
        self.assertIsNone(protocol.recv_frame(s))
        s.close()

    def test_half_close_without_requests(self):
        s = connect()
        s.shutdown(socket.SHUT_WR)
        self.assertIsNone(protocol.recv_frame(s))
        s.close()

    def test_half_close_inside_a_frame(self):
        # The frame can never be completed, so there is nothing to answer.
        s = connect()
        s.sendall(
            protocol.encode(ECHO, 1, b"whole")
            + protocol.encode(ECHO, 2, b"never finished")[:15]
        )
        s.shutdown(socket.SHUT_WR)
        self.assertEqual(protocol.recv_frame(s), (ECHO | RESPONSE, 1, b"whole"))
        self.assertIsNone(protocol.recv_frame(s))
        s.close()

    def test_error_reaches_a_client_that_keeps_sending(self):
        # A megabyte follows the bad length. If the server closed with
        # those bytes unread, the kernel would answer with a reset, which
        # can wipe out the ERROR frame before the client reads it.
        s = connect()
        s.settimeout(10)

        def send_ignoring_errors():
            try:
                s.sendall(struct.pack(">I", 3) + b"\xff" * (1024 * 1024))
            except OSError:
                pass

        sender = threading.Thread(target=send_ignoring_errors)
        sender.start()
        self.assert_error(
            protocol.recv_frame(s), protocol.ERR_MALFORMED_FRAME, 0
        )
        self.assertIsNone(protocol.recv_frame(s))
        sender.join()
        s.close()

    def test_reset_with_a_request_in_flight(self):
        # The worker finishes a request whose connection is already gone.
        # Its result must be dropped, not delivered to the next connection,
        # which usually gets the same descriptor number.
        #
        # The request is only as heavy as it needs to be to outlast the
        # pause. An abandoned request still occupies its worker, so heavy
        # ones would make the echo below wait its turn behind them.
        busy = protocol.encode(
            COMPUTE,
            1,
            protocol.compute_payload(
                protocol.MAX_COMPUTE_ITERATIONS, b"x" * 256
            ),
        )
        for _ in range(8):
            s = connect()
            s.sendall(busy)
            time.sleep(0.005)
            reset(s)
            fresh = connect()
            self.assert_echo(fresh, b"mine", request_id=77)
            fresh.close()

    def test_survives_abrupt_disconnects(self):
        # Clients that vanish with a request in flight, or halfway
        # through sending one.
        frame = protocol.encode(ECHO, 1, b"x" * 1000)
        for i in range(200):
            s = connect()
            s.sendall(frame if i % 2 == 0 else frame[: 5 + i])
            s.close()
        s = connect()
        self.assert_echo(s, b"still alive")
        s.close()

    def test_survives_reset_burst(self):
        # Some of these resets arrive before the server calls accept().
        for _ in range(200):
            reset(connect())
        s = connect()
        self.assert_echo(s, b"still alive")
        s.close()

    def test_many_clients(self):
        socks = [connect() for _ in range(200)]
        for i, s in enumerate(socks):
            s.sendall(protocol.encode(ECHO, i, f"client-{i:04d}".encode()))
        for i, s in enumerate(socks):
            self.assertEqual(
                protocol.recv_frame(s),
                (ECHO | RESPONSE, i, f"client-{i:04d}".encode()),
            )
            s.close()

    def test_sigint_stops_server(self):
        idle = connect()
        self.proc.send_signal(signal.SIGINT)
        self.assertEqual(self.proc.wait(timeout=2), 0)
        idle.close()


class FdLeakTest(ServerTestCase):
    def test_every_way_of_ending_a_connection_frees_its_descriptor(self):
        pid = self.proc.pid
        baseline = settled_fd_count(pid)
        frame = protocol.encode(ECHO, 1, b"x" * 1000)
        # Light enough that abandoned requests do not pile up on the
        # workers and delay the echoes in this loop.
        heavy = protocol.encode(
            COMPUTE, 1, protocol.compute_payload(2000, b"x" * 1024)
        )

        for i in range(300):
            s = connect()
            kind = i % 7

            if kind == 0:
                self.assert_echo(s, b"hello")
                s.close()
            elif kind == 1:
                s.close()
            elif kind == 2:
                s.sendall(frame[:9])
                s.close()
            elif kind == 3:
                s.sendall(heavy)
                reset(s)
            elif kind == 4:
                s.sendall(frame)
                s.shutdown(socket.SHUT_WR)
                self.assertEqual(protocol.recv_frame(s)[0], ECHO | RESPONSE)
                self.assertIsNone(protocol.recv_frame(s))
                s.close()
            elif kind == 5:
                s.sendall(struct.pack(">I", 2))
                self.assertEqual(protocol.recv_frame(s)[0], ERROR)
                self.assertIsNone(protocol.recv_frame(s))
                s.close()
            else:
                s.sendall(heavy)
                s.close()

        self.assertEqual(wait_for_fd_count(pid, baseline), baseline)
        self.assertIsNone(self.proc.poll())


class IdleTimeoutTest(ServerTestCase):
    TIMEOUT = 0.5
    OPTIONS = ("--workers", "4", "--idle-timeout-ms", "500")

    def assert_closed_by_timeout(self, s):
        elapsed = wait_closed(s, 5)
        self.assertIsNotNone(elapsed)
        self.assertGreater(elapsed, self.TIMEOUT - 0.1)
        self.assertLess(elapsed, self.TIMEOUT + 2)

    def test_idle_connection_is_closed(self):
        s = connect()
        self.assert_closed_by_timeout(s)
        s.close()

    def test_idle_connections_give_their_descriptors_back(self):
        baseline = settled_fd_count(self.proc.pid)
        socks = [connect() for _ in range(30)]
        for s in socks:
            self.assertIsNotNone(wait_closed(s, 5))
        self.assertEqual(wait_for_fd_count(self.proc.pid, baseline), baseline)
        for s in socks:
            s.close()

    def test_activity_restarts_the_timer(self):
        # Requests arrive more often than the timeout, for three times
        # its length. The connection has to survive that and then be
        # closed once it goes quiet.
        s = connect()
        for i in range(8):
            self.assertEqual(
                protocol.request(s, PING, i), (PING | RESPONSE, i, b"")
            )
            time.sleep(0.2)
        self.assert_echo(s, b"still here")
        self.assert_closed_by_timeout(s)
        s.close()

    def test_request_with_a_worker_is_not_idle(self):
        # Grow the work until one request takes well over twice the idle
        # timeout, and check that its answer still arrives.
        s = connect()
        s.settimeout(60)
        size = 256
        while True:
            start = time.time()
            reply = protocol.request(
                s,
                COMPUTE,
                size,
                protocol.compute_payload(
                    protocol.MAX_COMPUTE_ITERATIONS, b"x" * size
                ),
            )
            elapsed = time.time() - start
            self.assertEqual(reply[:2], (COMPUTE | RESPONSE, size))
            if elapsed > self.TIMEOUT * 2.5:
                break
            size *= 2
            self.assertLess(size, protocol.DEFAULT_MAX_FRAME // 2)
        s.close()


class PartialFrameTimeoutTest(ServerTestCase):
    TIMEOUT = 0.5
    OPTIONS = ("--workers", "4", "--partial-frame-timeout-ms", "500")

    def test_slowloris_is_cut_off(self):
        # One byte every 100 ms. Each byte is activity, but the frame is
        # no nearer to complete in any useful time. The last byte is never
        # sent, so the server can only have closed because of the timer.
        frame = protocol.encode(ECHO, 1, b"x" * 100)
        s = connect()
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        s.settimeout(0.1)

        start = time.time()
        closed_after = None
        sent = 0

        for i in range(len(frame) - 1):
            try:
                s.sendall(frame[i : i + 1])
                sent += 1
                # Doubles as the 100 ms pause between bytes.
                if s.recv(1) == b"":
                    closed_after = time.time() - start
                    break
                self.fail("the server answered an unfinished frame")
            except socket.timeout:
                continue
            except (ConnectionResetError, BrokenPipeError):
                closed_after = time.time() - start
                break

        self.assertIsNotNone(closed_after)
        self.assertGreater(closed_after, self.TIMEOUT - 0.1)
        self.assertLess(closed_after, self.TIMEOUT + 2)
        self.assertLess(sent, len(frame) - 1)
        s.close()

    def test_frame_finished_in_time_is_served(self):
        frame = protocol.encode(ECHO, 2, b"slow but fine")
        s = connect()
        s.sendall(frame[:6])
        time.sleep(0.2)
        s.sendall(frame[6:])
        self.assertEqual(
            protocol.recv_frame(s), (ECHO | RESPONSE, 2, b"slow but fine")
        )
        s.close()

    def test_timer_starts_again_for_each_frame(self):
        # Four frames, each taking 300 ms to arrive: 1.2 s in total,
        # more than twice the timeout, and none of them late.
        s = connect()
        for i in range(4):
            frame = protocol.encode(ECHO, i, b"frame")
            s.sendall(frame[:6])
            time.sleep(0.3)
            s.sendall(frame[6:])
            self.assertEqual(protocol.recv_frame(s), (ECHO | RESPONSE, i, b"frame"))
        s.close()


class WriteStallTimeoutTest(ServerTestCase):
    OPTIONS = (
        "--workers",
        "4",
        "--write-stall-timeout-ms",
        "500",
        "--output-high-water",
        "65536",
        "--max-frame",
        str(32 * 1024 * 1024),
    )

    def test_client_that_never_reads_is_cut_off(self):
        frame = protocol.encode(ECHO, 1, os.urandom(64 * 1024))
        baseline = settled_fd_count(self.proc.pid)

        slow = socket.socket()
        slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 64 * 1024)
        slow.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 64 * 1024)
        slow.connect(("127.0.0.1", PORT))
        slow.setblocking(False)

        # Send requests without reading until the socket refuses more.
        sent = 0
        blocked_since = None
        while sent < 64 * 1024 * 1024:
            try:
                sent += slow.send(frame[sent % len(frame) :])
                blocked_since = None
            except BlockingIOError:
                now = time.time()
                if blocked_since is None:
                    blocked_since = now
                if now - blocked_since > 0.3:
                    break
                time.sleep(0.01)
            except (ConnectionResetError, BrokenPipeError):
                break

        # The server has output it cannot deliver. Without reading a
        # byte, wait until it gives the connection up.
        self.assertEqual(
            wait_for_fd_count(self.proc.pid, baseline), baseline
        )

        # Reading now finds what the kernel already held, then the end.
        slow.setblocking(True)
        self.assertIsNotNone(wait_closed(slow, 5))
        slow.close()

        other = connect()
        self.assert_echo(other, b"not affected")
        other.close()

    def test_large_response_read_slowly_is_not_cut_off(self):
        # One 16 MiB response, read at 8 MiB/s. Sending it takes four
        # times the timeout and output is waiting the whole time, but it
        # keeps moving, so the timer must keep starting again.
        #
        # The rate is chosen with care. A sender only learns that the
        # reader has made room when the reader's kernel says so, and
        # macOS says so about once per half receive buffer. A reader
        # taking 32 KiB every 70 ms let the server send only every
        # 590 ms, which no server can tell from a stall. At 8 MiB/s half
        # a buffer is gone in tens of milliseconds.
        payload = os.urandom(16 * 1024 * 1024)
        rate = 8 * 1024 * 1024

        s = connect()
        s.settimeout(20)
        sender = threading.Thread(
            target=s.sendall, args=(protocol.encode(ECHO, 1, payload),)
        )
        sender.start()

        header = protocol.recv_exact(s, protocol.HEADER.size)
        self.assertEqual(
            protocol.HEADER.unpack(header),
            (8 + len(payload), 1, ECHO | RESPONSE, 0, 1),
        )

        # Paced by the clock, not by the length of each sleep, which is
        # more than twice as long on a busy CI machine as it is asked for.
        start = time.time()
        received = bytearray()
        while len(received) < len(payload):
            if len(received) > rate * (time.time() - start):
                time.sleep(0.005)
                continue
            try:
                chunk = s.recv(min(65536, len(payload) - len(received)))
            except ConnectionResetError:
                chunk = b""
            if not chunk:
                self.fail(
                    f"cut off after {len(received)} of {len(payload)} bytes "
                    f"and {time.time() - start:.2f} s, receive buffer "
                    f"{s.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)}"
                )
            received += chunk
        elapsed = time.time() - start
        sender.join()

        self.assertEqual(bytes(received), payload)
        self.assertGreater(elapsed, 1.5)
        s.close()


class LingerTimeoutTest(ServerTestCase):
    OPTIONS = ("--workers", "4", "--linger-timeout-ms", "500")

    def test_connection_is_closed_even_if_the_peer_never_closes(self):
        baseline = settled_fd_count(self.proc.pid)

        s = connect()
        s.sendall(struct.pack(">I", 1))
        self.assert_error(
            protocol.recv_frame(s), protocol.ERR_MALFORMED_FRAME, 0
        )
        self.assertIsNone(protocol.recv_frame(s))

        # The server has sent its FIN and is waiting for ours, which
        # never comes. Its descriptor must be freed all the same.
        self.assertEqual(wait_for_fd_count(self.proc.pid, baseline), baseline)
        s.close()


class LargeFrameTest(ServerTestCase):
    MAX_FRAME = 8 * 1024 * 1024
    OPTIONS = ("--workers", "4", "--max-frame", str(MAX_FRAME))

    def test_large_payload(self):
        # 4 MiB does not fit in the socket buffers, so the server has to
        # handle partial reads and partial writes.
        payload = os.urandom(4 * 1024 * 1024)
        s = connect()
        s.settimeout(20)
        sender = threading.Thread(
            target=s.sendall, args=(protocol.encode(ECHO, 1, payload),)
        )
        sender.start()
        reply = protocol.recv_frame(s)
        sender.join()
        self.assertEqual(reply, (ECHO | RESPONSE, 1, payload))
        s.close()

    def test_frame_limit_follows_the_option(self):
        s = connect()
        s.sendall(struct.pack(">I", self.MAX_FRAME + 1))
        self.assert_error(
            protocol.recv_frame(s), protocol.ERR_PAYLOAD_TOO_LARGE, 0
        )
        self.assertIsNone(protocol.recv_frame(s))
        s.close()


class SlowReaderTest(ServerTestCase):
    HIGH_WATER = 1024 * 1024
    OPTIONS = ("--workers", "4", "--output-high-water", str(HIGH_WATER))

    # How much the client offers before the test gives up waiting for the
    # server to stop reading.
    OFFERED = 64 * 1024 * 1024

    def test_client_that_never_reads_is_throttled(self):
        frame = protocol.encode(ECHO, 1, os.urandom(64 * 1024))
        rss_before = rss_bytes(self.proc.pid)

        # Small socket buffers on the client keep the kernel from hiding
        # megabytes of responses on the server's behalf.
        slow = socket.socket()
        slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 64 * 1024)
        slow.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 64 * 1024)
        slow.connect(("127.0.0.1", PORT))
        slow.setblocking(False)

        # Send requests and never read a response. Stop once the socket
        # has refused more data for a full second.
        sent = 0
        blocked_since = None
        while sent < self.OFFERED:
            try:
                sent += slow.send(frame[sent % len(frame) :])
                blocked_since = None
            except BlockingIOError:
                now = time.time()
                if blocked_since is None:
                    blocked_since = now
                if now - blocked_since > 1:
                    break
                time.sleep(0.01)

        # The server stopped taking requests long before the client ran
        # out. Whatever it holds, it holds at most what was sent.
        self.assertLess(sent, self.OFFERED)

        grown = rss_bytes(self.proc.pid) - rss_before
        self.assertLess(grown, 256 * 1024 * 1024)

        # Everyone else is served as usual.
        other = connect()
        for i in range(50):
            self.assert_echo(other, b"not affected", request_id=i)
        self.assertEqual(protocol.request(other, PING, 99), (PING | RESPONSE, 99, b""))
        other.close()

        # When the slow client starts reading, it gets an intact response
        # for every request it managed to send, in order.
        slow.setblocking(True)
        slow.settimeout(20)

        whole_frames, partial = divmod(sent, len(frame))
        finisher = None
        if partial:
            whole_frames += 1
            finisher = threading.Thread(
                target=slow.sendall, args=(frame[partial:],)
            )
            finisher.start()

        expected = (ECHO | RESPONSE, 1, frame[protocol.HEADER.size :])
        for _ in range(whole_frames):
            self.assertEqual(protocol.recv_frame(slow), expected)

        if finisher:
            finisher.join()
        self.assert_echo(slow, b"caught up")
        slow.close()


class ConnectionLimitTest(ServerTestCase):
    LIMIT = 5
    OPTIONS = ("--workers", "4", "--max-connections", str(LIMIT))

    def fill_to_limit(self):
        held = [connect() for _ in range(self.LIMIT)]
        for s in held:
            self.assert_echo(s, b"held")
        return held

    def test_limit_is_enforced_and_recovers(self):
        held = self.fill_to_limit()

        # The kernel completes the handshake on its own, so connect()
        # succeeds. The server must leave this connection in the backlog.
        extra = connect()
        extra.sendall(protocol.encode(ECHO, 1, b"extra"))
        extra.settimeout(0.5)
        with self.assertRaises(socket.timeout):
            extra.recv(1)

        self.assert_echo(held[1], b"again")

        held[0].close()
        extra.settimeout(5)
        self.assertEqual(protocol.recv_frame(extra), (ECHO | RESPONSE, 1, b"extra"))

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
                    if not try_echo(s, message, request_id=r):
                        raise AssertionError("no echo")
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
