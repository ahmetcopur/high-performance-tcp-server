"""
Stress and fault tests. Each one puts the real server under one kind of
abuse, at a scale the integration tests do not, and then checks three
things by name:

  - while it lasted, well-behaved clients were served correctly,
  - afterwards the server holds nothing it did not hold before:
    no connection, no descriptor, no request in its queue,
  - asked to stop, it stops in an orderly way, with exit code 0, and its
    port is free at once.

Usage:
    python3 tests/test_stress.py path/to/server [--poller poll|epoll]
"""

import concurrent.futures
import os
import random
import signal
import socket
import struct
import sys
import threading
import time
import unittest

import protocol
import test_server as base
from protocol import COMPUTE, ECHO, PING, RESPONSE, STATS

# Not the integration suite's port, so that a server left over from one
# cannot be mistaken for the other's.
base.PORT = 18084

ADDRESS = ("127.0.0.1", base.PORT)


def connect(timeout=10):
    s = socket.create_connection(ADDRESS, timeout=timeout)
    s.settimeout(timeout)
    return s


def stats(s, request_id=1):
    return protocol.parse_stats(protocol.request(s, STATS, request_id))


def run_all(count, target, *args):
    # Runs target(i, *args) on `count` threads and returns what each
    # returned. An exception in a thread fails the test that called.
    with concurrent.futures.ThreadPoolExecutor(max_workers=count) as pool:
        futures = [pool.submit(target, i, *args) for i in range(count)]
        return [f.result() for f in futures]


class GoodClient(threading.Thread):
    """
    A well-behaved client that keeps asking until told to stop, and checks
    every answer. What it saw is what "served correctly meanwhile" means.
    """

    def __init__(self, name):
        super().__init__(daemon=True)
        self.label = name.encode()
        self.stop = threading.Event()
        self.answers = 0
        self.slowest = 0.0
        self.failure = None

    def run(self):
        try:
            s = connect()
            i = 0
            while not self.stop.is_set():
                i += 1
                payload = b"%s %d" % (self.label, i)
                started = time.monotonic()
                reply = protocol.request(s, ECHO, i, payload)
                self.slowest = max(self.slowest, time.monotonic() - started)
                if reply != (ECHO | RESPONSE, i, payload):
                    raise AssertionError(f"wrong answer: {reply!r}")
                self.answers += 1
                time.sleep(0.002)
            s.close()
        except Exception as error:  # reported by finish()
            self.failure = error

    def finish(self, test, slowest_allowed):
        self.stop.set()
        self.join(30)
        test.assertFalse(self.is_alive(), "a good client is stuck")
        test.assertIsNone(self.failure)
        test.assertGreater(self.answers, 0)
        test.assertLess(self.slowest, slowest_allowed)


class StressTestCase(base.ServerTestCase):
    def setUp(self):
        super().setUp()
        self.fds_before = base.settled_fd_count(self.proc.pid)
        self.rss_before = base.rss_bytes(self.proc.pid)

    def good_clients(self, count):
        clients = [GoodClient(f"good-{i}") for i in range(count)]
        for client in clients:
            client.start()
        return clients

    def assert_nothing_left_behind(self):
        pid = self.proc.pid
        self.assertIsNone(self.proc.poll(), "the server is gone")

        # Every descriptor is back. This waits, because the server only
        # learns of a closed connection when it next looks at it.
        self.assertEqual(
            base.wait_for_fd_count(pid, self.fds_before, timeout=15),
            self.fds_before,
            "descriptors left open",
        )

        # It serves, and by its own count it holds one connection, this
        # one, and no request.
        s = connect()
        self.assert_echo(s, b"after the storm")

        deadline = time.time() + 10
        while True:
            now = stats(s)
            idle = (
                now["connections"] == 1
                and now["queue_depth"] == 0
                and now["tasks_completed"] == now["tasks_submitted"]
            )
            if idle or time.time() > deadline:
                break
            time.sleep(0.05)

        self.assertEqual(now["connections"], 1)
        self.assertEqual(now["queue_depth"], 0)
        self.assertEqual(now["tasks_completed"], now["tasks_submitted"])
        self.assertEqual(now["tasks_failed"], 0)
        s.close()

        # Not a leak check, the soak test is that. This catches a server
        # that kept hundreds of megabytes. The room is for a sanitizer,
        # which holds on to freed memory for a while on purpose.
        grown = base.rss_bytes(pid) - self.rss_before
        self.assertLess(grown, 512 * 1024 * 1024)

        return now

    def assert_stops_cleanly(self, sig=signal.SIGTERM):
        self.proc.send_signal(sig)
        self.assertEqual(self.proc.wait(timeout=15), 0)

        # The port can be bound again at once.
        listener = socket.socket()
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(ADDRESS)
        listener.close()


class ConnectionBurstTest(StressTestCase):
    LIMIT = 50
    OPTIONS = ("--workers", "4", "--max-connections", str(LIMIT))

    def test_six_times_the_limit_at_once_are_all_served(self):
        watcher = connect()
        most = [0]
        done = threading.Event()
        full = threading.Event()

        def watch():
            i = 0
            while not done.is_set():
                i += 1
                most[0] = max(most[0], stats(watcher, i)["connections"])
                if most[0] >= self.LIMIT:
                    full.set()
                time.sleep(0.005)

        watching = threading.Thread(target=watch, daemon=True)
        watching.start()

        def client(i):
            # Those beyond the limit wait in the backlog for a slot.
            s = connect(timeout=30)
            for j in range(5):
                payload = b"burst %d %d" % (i, j)
                if protocol.request(s, ECHO, j, payload) != (ECHO | RESPONSE, j, payload):
                    return False
                # The first ones in stay until the server is full, so
                # that the limit is reached however fast it serves.
                full.wait(20)
            s.close()
            return True

        results = run_all(6 * self.LIMIT, client)

        done.set()
        watching.join(10)
        watcher.close()

        self.assertEqual(results.count(True), 6 * self.LIMIT)

        # The limit was reached, and never passed.
        self.assertEqual(most[0], self.LIMIT)

        self.assert_nothing_left_behind()
        self.assert_stops_cleanly()


class ChurnTest(StressTestCase):
    THREADS = 8
    CYCLES = 300

    def test_connections_that_come_and_go_in_every_way(self):
        frame = protocol.encode(ECHO, 7, b"x" * 100)
        large = protocol.encode(ECHO, 8, b"y" * 200000)
        good = self.good_clients(2)

        def churn(i):
            rng = random.Random(i)
            answered = 0

            for _ in range(self.CYCLES):
                way = rng.randrange(8)
                s = connect()

                try:
                    if way == 0:
                        pass  # connect and leave
                    elif way == 1:
                        s.sendall(frame)  # ask and leave without the answer
                    elif way == 2:
                        s.sendall(frame[: rng.randrange(1, len(frame))])  # mid-frame
                    elif way == 3:
                        s.sendall(frame)
                        base.reset(s)  # reset with a request in flight
                        continue
                    elif way == 4:
                        s.sendall(large[: rng.randrange(1, len(large))])
                        base.reset(s)  # reset inside a large frame
                        continue
                    elif way == 5:
                        s.sendall(frame * 20)  # a pipeline, abandoned
                    elif way == 6:
                        if protocol.request(s, ECHO, 7, b"x" * 100) != (
                            ECHO | RESPONSE, 7, b"x" * 100
                        ):
                            raise AssertionError("wrong answer")
                        answered += 1
                    else:
                        s.sendall(frame)
                        s.shutdown(socket.SHUT_WR)  # half-close, then the answer
                        if protocol.recv_frame(s) != (ECHO | RESPONSE, 7, b"x" * 100):
                            raise AssertionError("wrong answer after half-close")
                        if protocol.recv_frame(s) is not None:
                            raise AssertionError("more than one answer")
                        answered += 1
                except (ConnectionResetError, BrokenPipeError):
                    # The server may have closed first. Only for the ways
                    # that leave without reading.
                    if way in (6, 7):
                        raise

                s.close()

            return answered

        answered = run_all(self.THREADS, churn)
        self.assertGreater(sum(answered), self.THREADS * self.CYCLES // 8)

        for client in good:
            client.finish(self, slowest_allowed=5)

        self.assert_nothing_left_behind()
        self.assert_stops_cleanly()


class SlowReadersTest(StressTestCase):
    HIGH_WATER = 256 * 1024
    OPTIONS = (
        "--workers", "4",
        "--output-high-water", str(HIGH_WATER),
        "--write-stall-timeout-ms", "1500",
    )
    READERS = 20

    def test_many_clients_that_never_read(self):
        frame = protocol.encode(ECHO, 1, os.urandom(64 * 1024))
        good = self.good_clients(4)
        slow = []

        for _ in range(self.READERS):
            s = socket.socket()
            # Small buffers, so the kernel cannot hide megabytes of
            # answers on the server's behalf.
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 64 * 1024)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 64 * 1024)
            s.connect(ADDRESS)
            s.setblocking(False)
            slow.append(s)

        # Each sends until the server will take no more, and never reads.
        sent = [0] * self.READERS
        quiet_since = None
        deadline = time.time() + 20

        while time.time() < deadline:
            moved = False
            for i, s in enumerate(slow):
                try:
                    n = s.send(frame[sent[i] % len(frame):])
                    sent[i] += n
                    moved = moved or n > 0
                except (BlockingIOError, ConnectionResetError, BrokenPipeError):
                    pass
            if moved:
                quiet_since = None
            else:
                quiet_since = quiet_since or time.time()
                if time.time() - quiet_since > 0.5:
                    break
                time.sleep(0.01)

        # Every one of them was stopped: none got to send without end.
        self.assertIsNotNone(quiet_since)

        # What the server holds for them is bounded by its limits, not by
        # what they offered. Per connection: the output up to its
        # watermark plus a response, and one frame of input.
        held = base.rss_bytes(self.proc.pid) - self.rss_before
        self.assertLess(held, 256 * 1024 * 1024)

        # It then gives up on them: they take nothing of what they are owed.
        for s in slow:
            s.setblocking(True)
            # Not reading here either. Only wait for the server to act.
        deadline = time.time() + 15
        open_fds = base.count_fds(self.proc.pid)
        while open_fds > self.fds_before + len(good) and time.time() < deadline:
            time.sleep(0.1)
            open_fds = base.count_fds(self.proc.pid)
        self.assertLessEqual(open_fds, self.fds_before + len(good))

        for client in good:
            client.finish(self, slowest_allowed=5)

        for s in slow:
            s.close()

        self.assert_nothing_left_behind()
        self.assert_stops_cleanly()


class SlowlorisTest(StressTestCase):
    LIMIT_MS = 1000
    OPTIONS = ("--workers", "4", "--partial-frame-timeout-ms", str(LIMIT_MS))
    WRITERS = 200

    def test_many_clients_that_never_finish_a_frame(self):
        good = self.good_clients(4)
        frame = protocol.encode(ECHO, 1, b"z" * 4000)
        writers = [connect() for _ in range(self.WRITERS)]
        cut_off_after = [None] * self.WRITERS
        started = time.monotonic()

        for s in writers:
            s.setblocking(False)

        # One byte each, five times a second. Fast enough to never look
        # idle, far too slow to finish a frame.
        position = 0
        while time.monotonic() - started < 10 and None in cut_off_after:
            position += 1
            for i, s in enumerate(writers):
                if cut_off_after[i] is not None:
                    continue
                try:
                    s.send(frame[position - 1:position])
                    if s.recv(1) == b"":
                        cut_off_after[i] = time.monotonic() - started
                except BlockingIOError:
                    pass
                except (ConnectionResetError, BrokenPipeError):
                    cut_off_after[i] = time.monotonic() - started
            time.sleep(0.2)

        self.assertNotIn(None, cut_off_after, "a slow writer was left alone")

        # Not before the limit, and not long after it.
        self.assertGreater(min(cut_off_after), self.LIMIT_MS / 1000 * 0.8)
        self.assertLess(max(cut_off_after), self.LIMIT_MS / 1000 + 3)

        for client in good:
            client.finish(self, slowest_allowed=5)

        for s in writers:
            s.close()

        self.assert_nothing_left_behind()
        self.assert_stops_cleanly()


class QueueSaturationTest(StressTestCase):
    OPTIONS = ("--workers", "2", "--queue-capacity", "4")
    CLIENTS = 40
    SECONDS = 3

    def test_heavy_compute_from_far_more_clients_than_the_queue_holds(self):
        data = bytes(range(256))
        iterations = 2000
        request = protocol.compute_payload(iterations, data)
        expected = struct.pack(">Q", protocol.compute_reference(data, iterations))

        # PING is answered by the event loop. It must stay quick while
        # every worker is busy and the queue is full.
        probe = connect()
        slowest_ping = [0.0]
        done = threading.Event()

        def ping():
            i = 0
            while not done.is_set():
                i += 1
                started = time.monotonic()
                if protocol.request(probe, PING, i) != (PING | RESPONSE, i, b""):
                    raise AssertionError("wrong answer to PING")
                slowest_ping[0] = max(slowest_ping[0], time.monotonic() - started)
                time.sleep(0.01)

        pinging = threading.Thread(target=ping, daemon=True)
        pinging.start()

        def client(i):
            s = connect()
            computed = busy = 0
            until = time.monotonic() + self.SECONDS
            n = 0

            while time.monotonic() < until:
                n += 1
                reply = protocol.request(s, COMPUTE, n, request)
                if reply == (COMPUTE | RESPONSE, n, expected):
                    computed += 1
                elif (reply[1] == n
                      and protocol.error_code(reply) == protocol.ERR_SERVER_BUSY):
                    busy += 1
                else:
                    raise AssertionError(f"neither an answer nor busy: {reply!r}")

            s.close()
            return computed, busy

        results = run_all(self.CLIENTS, client)

        done.set()
        pinging.join(10)
        probe.close()

        computed = sum(r[0] for r in results)
        busy = sum(r[1] for r in results)

        # Both happened, and every client got some work done.
        self.assertGreater(busy, 0)
        self.assertGreater(computed, 0)
        self.assertLess(slowest_ping[0], 2)

        # The server's count agrees with what the clients saw, request
        # for request.
        after = self.assert_nothing_left_behind()
        self.assertEqual(after["tasks_rejected"], busy)
        # One more: the echo in assert_nothing_left_behind().
        self.assertEqual(after["tasks_submitted"], computed + 1)

        self.assert_stops_cleanly()


class DescriptorExhaustionTest(StressTestCase):
    FD_LIMIT = 64
    OPTIONS = ("--workers", "4")
    ATTEMPTS = 300

    def test_a_storm_of_connections_with_no_descriptors_to_spare(self):
        # These are in before the storm and must not notice it.
        good = self.good_clients(4)
        time.sleep(0.2)

        def attempt(i):
            # Served, or turned away at once. Never left hanging.
            try:
                s = connect(timeout=10)
                s.settimeout(10)
                served = base.try_echo(s, b"storm %d" % i)
                s.close()
                return "served" if served else "refused"
            except (ConnectionResetError, BrokenPipeError, ConnectionRefusedError):
                return "refused"
            except socket.timeout:
                return "hung"

        # Use the descriptors up: connections that stay, until one more
        # is turned away.
        held = []
        while True:
            s = connect()
            if not base.try_echo(s, b"held"):
                s.close()
                break
            held.append(s)
            self.assertLess(len(held), self.FD_LIMIT)

        # The storm, with nothing to spare. Nobody can be served, and
        # nobody may be left waiting.
        outcomes = run_all(self.ATTEMPTS, attempt) + run_all(self.ATTEMPTS, attempt)
        self.assertEqual(outcomes.count("hung"), 0)
        self.assertEqual(outcomes.count("refused"), 2 * self.ATTEMPTS)

        # Those who were in before it are still served.
        for s in held[:5]:
            self.assertTrue(base.try_echo(s, b"still here"))

        for s in held:
            s.close()

        # With descriptors free again, the same storm is served. A
        # connection can still be turned away when too many arrive in
        # the same instant.
        outcomes = run_all(self.ATTEMPTS, attempt)
        self.assertEqual(outcomes.count("hung"), 0)
        self.assertGreater(outcomes.count("served"), 0)

        for client in good:
            client.finish(self, slowest_allowed=5)

        self.assert_nothing_left_behind()
        self.assert_stops_cleanly()


class ShutdownUnderLoadTest(StressTestCase):
    OPTIONS = ("--workers", "4")
    CLIENTS = 50

    def test_every_client_sees_whole_answers_and_then_a_clean_end(self):
        stopping = threading.Event()

        def client(i):
            # Returns how many answers it got. The connection may end at
            # any time once the server is stopping, but only between two
            # frames, and never with a wrong answer.
            s = connect()
            answers = 0
            n = 0

            while True:
                n += 1
                payload = b"load %d %d" % (i, n)
                try:
                    reply = protocol.request(s, ECHO, n, payload)
                except (ConnectionResetError, BrokenPipeError):
                    # Only once the server is stopping, and only because
                    # a request was sent into a connection it had closed.
                    if not stopping.is_set():
                        raise
                    break

                # recv_frame() raises if the stream ends inside a frame.
                if reply is None:
                    if not stopping.is_set():
                        raise AssertionError("closed before the server was asked to stop")
                    break

                if reply != (ECHO | RESPONSE, n, payload):
                    raise AssertionError(f"wrong answer: {reply!r}")
                answers += 1

            s.close()
            return answers

        with concurrent.futures.ThreadPoolExecutor(max_workers=self.CLIENTS) as pool:
            futures = [pool.submit(client, i) for i in range(self.CLIENTS)]
            time.sleep(1.5)

            stopping.set()
            self.proc.send_signal(signal.SIGTERM)
            exit_code = self.proc.wait(timeout=15)

            answers = [f.result(timeout=30) for f in futures]

        self.assertEqual(exit_code, 0)
        self.assertTrue(all(count > 0 for count in answers))

        listener = socket.socket()
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(ADDRESS)
        listener.close()


class RestartTest(unittest.TestCase):
    CYCLES = 25

    def test_stopped_and_started_again_and_again_with_requests_in_flight(self):
        # Long enough to be under way when the signal comes: tens of
        # milliseconds each, eight of them for four workers.
        request = protocol.encode(
            COMPUTE, 1, protocol.compute_payload(20000, bytes(range(256)) * 4)
        )
        expected = None

        for cycle in range(self.CYCLES):
            # Starting fails if the port is still held from the last
            # round, or if anything else was left behind.
            proc = base.start_server(
                "--workers", "4", "--shutdown-timeout-ms", "30000"
            )

            try:
                control = connect()

                # What an undisturbed request is answered with. Whether
                # that is the right number is tested elsewhere.
                if expected is None:
                    control.sendall(request)
                    expected = protocol.recv_frame(control)
                    self.assertEqual(expected[0], COMPUTE | RESPONSE)

                handed_over = stats(control)["tasks_submitted"]
                clients = [connect() for _ in range(8)]
                for s in clients:
                    s.sendall(request)

                # Stop the server once it has taken all eight, and not
                # before: a request it has not read yet is not promised
                # an answer.
                deadline = time.time() + 10
                while stats(control)["tasks_submitted"] < handed_over + 8:
                    self.assertLess(time.time(), deadline)
                    time.sleep(0.001)

                proc.send_signal(signal.SIGTERM if cycle % 2 else signal.SIGINT)

                # What was started is finished and answered, and then the
                # connection ends without a reset.
                for s in clients:
                    s.settimeout(30)
                    self.assertEqual(protocol.recv_frame(s), expected, f"cycle {cycle}")
                    self.assertIsNone(protocol.recv_frame(s))
                    s.close()

                # The idle one is simply closed.
                self.assertIsNone(protocol.recv_frame(control))
                control.close()

                self.assertEqual(proc.wait(timeout=30), 0, f"cycle {cycle}")
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()


class EverythingAtOnceTest(StressTestCase):
    OPTIONS = (
        "--workers", "4",
        "--max-connections", "200",
        "--partial-frame-timeout-ms", "1000",
        "--write-stall-timeout-ms", "1500",
        "--output-high-water", str(256 * 1024),
    )
    SECONDS = 6

    def test_all_of_it_together(self):
        good = self.good_clients(6)
        until = time.monotonic() + self.SECONDS
        frame = protocol.encode(ECHO, 1, b"q" * 2000)
        large = protocol.encode(ECHO, 2, os.urandom(64 * 1024))
        heavy = protocol.encode(
            COMPUTE, 3, protocol.compute_payload(5000, bytes(1024))
        )

        def misbehave(i):
            rng = random.Random(1000 + i)

            while time.monotonic() < until:
                way = rng.randrange(6)
                try:
                    s = connect()
                    if way == 0:
                        base.reset(s)
                        continue
                    if way == 1:
                        s.sendall(frame[: rng.randrange(1, len(frame))])
                        time.sleep(rng.random() * 0.3)
                    elif way == 2:
                        # Asks for a great deal and reads none of it.
                        s.setblocking(False)
                        for _ in range(40):
                            try:
                                s.send(large)
                            except BlockingIOError:
                                break
                        time.sleep(rng.random() * 0.5)
                    elif way == 3:
                        # Occupies workers and leaves.
                        s.sendall(heavy * 4)
                        time.sleep(rng.random() * 0.2)
                    elif way == 4:
                        s.sendall(struct.pack(">I", rng.choice([0, 3, 7, 2**31])))
                        time.sleep(0.05)
                    else:
                        s.sendall(os.urandom(rng.randrange(1, 4096)))
                        time.sleep(0.05)
                    s.close()
                except (ConnectionResetError, BrokenPipeError, BlockingIOError,
                        socket.timeout, ConnectionRefusedError):
                    pass

            return True

        run_all(24, misbehave)

        # Through all of it, the well-behaved clients were served, and
        # correctly. A request may have waited behind heavy work.
        for client in good:
            client.finish(self, slowest_allowed=10)

        self.assert_nothing_left_behind()
        self.assert_stops_cleanly()


if __name__ == "__main__":
    if len(sys.argv) > 1:
        base.SERVER = sys.argv.pop(1)
    if "--poller" in sys.argv:
        at = sys.argv.index("--poller")
        base.SERVER_OPTIONS = sys.argv[at:at + 2]
        del sys.argv[at:at + 2]
    unittest.main()
