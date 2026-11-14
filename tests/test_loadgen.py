"""
Tests for the load generator. They run it against the real server and
check that what it reports is what happened.

Usage:
    python3 tests/test_loadgen.py path/to/server path/to/loadgen
"""

import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import unittest

SERVER = "./server"
LOADGEN = "./loadgen"
PORT = 18082


def wait_until_listening():
    deadline = time.time() + 5
    while True:
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=1).close()
            return
        except OSError:
            if time.time() > deadline:
                raise
            time.sleep(0.02)


class LoadgenTestCase(unittest.TestCase):
    SERVER_OPTIONS = ("--workers", "4")

    def setUp(self):
        self.server = subprocess.Popen(
            [SERVER, "--port", str(PORT), *self.SERVER_OPTIONS],
            stdout=subprocess.DEVNULL,
        )
        wait_until_listening()

    def tearDown(self):
        if self.server.poll() is None:
            self.server.kill()
        self.server.wait()

    def loadgen(self, *options):
        # Runs the load generator and returns its result.
        with tempfile.TemporaryDirectory() as directory:
            output = os.path.join(directory, "result.json")
            completed = subprocess.run(
                [LOADGEN, "--port", str(PORT), "--output", output, *options],
                capture_output=True,
                text=True,
                timeout=120,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            with open(output) as f:
                return json.load(f)

    def assert_nothing_failed(self, result):
        requests = result["requests"]
        for kind in ("busy", "errors", "timeouts", "connection_errors"):
            self.assertEqual(requests[kind], 0, kind)
        self.assertEqual(result["connections"]["connect_failures"], 0)


class ClosedLoopTest(LoadgenTestCase):
    def test_reports_what_it_did(self):
        result = self.loadgen(
            "--connections", "8", "--threads", "2",
            "--warmup", "0.3", "--duration", "1",
            "--type", "echo", "--payload", "200",
        )

        self.assert_nothing_failed(result)
        self.assertEqual(result["config"]["mode"], "closed")
        self.assertEqual(result["connections"]["connected"], 8)
        self.assertEqual(result["config"]["request_bytes"], 212)
        self.assertEqual(result["config"]["response_bytes"], 212)
        self.assertAlmostEqual(result["measured_s"], 1.0, delta=0.2)

        completed = result["requests"]["completed"]
        self.assertGreater(completed, 100)
        # measured_s is written with three decimals, so allow for that.
        self.assertAlmostEqual(
            result["throughput_rps"],
            completed / result["measured_s"],
            delta=result["throughput_rps"] * 0.005,
        )

        latency = result["latency_ns"]
        self.assertEqual(latency["count"], completed)
        self.assertLessEqual(latency["min"], latency["p50"])
        self.assertLessEqual(latency["p50"], latency["p99"])
        self.assertLessEqual(latency["p99"], latency["max"])
        self.assertEqual(latency["counted_from"], "when the request was sent")

    def test_bytes_are_counted_at_the_socket(self):
        # Every request is 12 + 1000 bytes each way. The totals may differ
        # from the completed requests only by the few that were under way
        # when measuring started or stopped.
        result = self.loadgen(
            "--connections", "4", "--warmup", "0.2", "--duration", "1",
            "--type", "echo", "--payload", "1000",
        )

        self.assert_nothing_failed(result)
        completed = result["requests"]["completed"]

        for direction in ("sent", "received"):
            self.assertAlmostEqual(
                result["bytes"][direction] / 1012, completed, delta=8
            )

    def test_histogram_holds_every_sample(self):
        result = self.loadgen("--connections", "4", "--warmup", "0.2", "--duration", "1")

        self.assertEqual(
            sum(count for _, count in result["histogram"]),
            result["latency_ns"]["count"],
        )
        tops = [top for top, _ in result["histogram"]]
        self.assertEqual(tops, sorted(tops))
        self.assertGreaterEqual(tops[-1], result["latency_ns"]["max"])

    def test_percentiles_without_enough_samples_are_left_out(self):
        # A handful of slow requests: enough for a median, not for more.
        result = self.loadgen(
            "--connections", "1", "--warmup", "0", "--duration", "1",
            "--type", "compute", "--iterations", "100000", "--payload", "256",
        )

        latency = result["latency_ns"]
        self.assertLess(latency["count"], 1000)
        self.assertIsNone(latency["p99"])
        self.assertIsNone(latency["p999"])

    def test_ping_and_compute(self):
        ping = self.loadgen("--connections", "2", "--warmup", "0.2", "--duration", "0.5", "--type", "ping")
        compute = self.loadgen(
            "--connections", "2", "--warmup", "0.2", "--duration", "0.5",
            "--type", "compute", "--iterations", "10", "--payload", "64",
        )

        for result in (ping, compute):
            self.assert_nothing_failed(result)
            self.assertGreater(result["requests"]["completed"], 10)

        self.assertEqual(ping["config"]["request_bytes"], 12)
        self.assertEqual(ping["config"]["response_bytes"], 12)
        self.assertEqual(compute["config"]["request_bytes"], 12 + 4 + 64)
        self.assertEqual(compute["config"]["response_bytes"], 20)

    def test_idle_connections_are_opened_and_left_alone(self):
        result = self.loadgen(
            "--connections", "2", "--idle-connections", "30",
            "--warmup", "0.2", "--duration", "0.5",
        )

        self.assert_nothing_failed(result)
        self.assertEqual(result["connections"]["connected"], 32)
        self.assertEqual(result["connections"]["idle_lost"], 0)


class OpenLoopTest(LoadgenTestCase):
    def test_keeps_the_rate_it_was_given(self):
        result = self.loadgen(
            "--connections", "8", "--threads", "2",
            "--warmup", "0.3", "--duration", "2", "--rate", "1000",
        )

        self.assert_nothing_failed(result)
        self.assertEqual(result["config"]["mode"], "open")
        self.assertAlmostEqual(result["throughput_rps"], 1000, delta=30)
        self.assertEqual(result["requests"]["not_sent"], 0)
        self.assertEqual(
            result["latency_ns"]["counted_from"], "when the request was due"
        )
        self.assertAlmostEqual(result["client"]["scheduled"], 2000, delta=40)


class CoordinatedOmissionTest(LoadgenTestCase):
    # One worker and a request that takes it tens of milliseconds, so the
    # server can do a few dozen a second at most.
    SERVER_OPTIONS = ("--workers", "1")
    SLOW = ("--type", "compute", "--iterations", "100000", "--payload", "512")

    def test_open_loop_counts_the_time_a_request_waited_to_be_sent(self):
        closed = self.loadgen(
            "--connections", "1", "--warmup", "0", "--duration", "2", *self.SLOW
        )
        capacity = closed["throughput_rps"]
        self.assertGreater(closed["requests"]["completed"], 3)

        # Ask for four times what the server can do. Requests fall due
        # while the connection is still waiting, and pile up.
        opened = self.loadgen(
            "--connections", "1", "--warmup", "0", "--duration", "2",
            "--rate", str(capacity * 4), *self.SLOW,
        )

        # The server did the same work at the same speed in both runs.
        self.assertAlmostEqual(
            opened["throughput_rps"], capacity, delta=capacity * 0.5
        )

        # But a client that keeps to its schedule waited far longer for
        # its answers, and many requests were never sent at all. A closed
        # loop cannot see either.
        self.assertGreater(
            opened["latency_ns"]["p50"], 5 * closed["latency_ns"]["p50"]
        )
        self.assertGreater(opened["requests"]["not_sent"], 0)


class FailureCountingTest(LoadgenTestCase):
    SERVER_OPTIONS = ("--workers", "1", "--queue-capacity", "1")

    def test_busy_answers_are_counted_apart_from_completed_ones(self):
        result = self.loadgen(
            "--connections", "8", "--warmup", "0.2", "--duration", "1",
            "--type", "compute", "--iterations", "20000", "--payload", "512",
        )

        self.assertGreater(result["requests"]["busy"], 0)
        self.assertGreater(result["requests"]["completed"], 0)
        self.assertEqual(result["requests"]["errors"], 0)

    def test_requests_without_an_answer_in_time_are_timeouts(self):
        result = self.loadgen(
            "--connections", "1", "--warmup", "0", "--duration", "2",
            "--timeout-ms", "200",
            "--type", "compute", "--iterations", "100000", "--payload", "65536",
        )

        self.assertEqual(result["requests"]["timeouts"], 1)
        self.assertEqual(result["requests"]["completed"], 0)


class CommandLineTest(unittest.TestCase):
    def test_bad_options_print_usage(self):
        for options in (["--connections", "0"], ["--type", "nonsense"], ["--bogus", "1"], ["--port"]):
            completed = subprocess.run(
                [LOADGEN, *options], capture_output=True, text=True, timeout=10
            )
            self.assertEqual(completed.returncode, 2, options)
            self.assertIn("Usage:", completed.stderr)

    def test_reports_connections_it_could_not_make(self):
        # Nothing listens on this port.
        with tempfile.TemporaryDirectory() as directory:
            output = os.path.join(directory, "result.json")
            completed = subprocess.run(
                [LOADGEN, "--port", "18083", "--connections", "3",
                 "--warmup", "0", "--duration", "0.2", "--output", output],
                capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            with open(output) as f:
                result = json.load(f)

        self.assertEqual(result["connections"]["connect_failures"], 3)
        self.assertEqual(result["connections"]["connected"], 0)
        self.assertEqual(result["requests"]["completed"], 0)


if __name__ == "__main__":
    if len(sys.argv) > 2:
        SERVER = sys.argv.pop(1)
        LOADGEN = sys.argv.pop(1)
    unittest.main()
