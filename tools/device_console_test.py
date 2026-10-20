import contextlib
import io
import sys
import unittest
from unittest.mock import patch

import device_console as console


class ConsoleTests(unittest.TestCase):
    def run_command(self, command, payload, disconnect=False, timeout="3", delayed_payload=None):
        clock = [0.0]

        def now():
            clock[0] += 0.2
            return clock[0]

        class Port:
            in_waiting = 0

            def __init__(self):
                self.payload = payload
                self.commands = []

            def open(self):
                assert self.dtr is False and self.rts is False

            def reset_input_buffer(self):
                pass

            def write(self, data):
                self.commands.append(data)

            def read(self, _):
                if self.payload:
                    result, self.payload = self.payload, b""
                    return result
                if delayed_payload and clock[0] >= 4 and not getattr(self, "delivered_late", False):
                    self.delivered_late = True
                    return delayed_payload
                if disconnect:
                    raise console.serial.SerialException("test disconnect")
                return b""

            def __enter__(self):
                return self

            def __exit__(self, *_):
                pass

        port = Port()
        argv = ["console", command] + (["--timeout", timeout] if timeout else [])
        with patch.object(sys, "argv", argv), \
             patch.object(console.serial, "Serial", return_value=port), \
             patch.object(console.time, "sleep"), \
             patch.object(console.time, "monotonic", side_effect=now), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            result = console.main()
        self.last_elapsed = clock[0]
        self.assertEqual(port.commands, [(command + "\n").encode()])
        return result

    def test_selftest_requires_explicit_success(self):
        self.assertEqual(self.run_command("power-selftest 1", b"[PowerTestDone] cases=8 passed=8 failed=0\n[PowerTestResult] success=1\n"), 0)

    def test_preflight_zero_cases_is_not_a_pass(self):
        self.assertEqual(self.run_command("power-selftest 30", b"[PowerTestDone] cases=0 passed=0 failed=0\n[PowerTestResult] success=0\n"), 2)

    def test_timeout_is_not_a_pass(self):
        self.assertEqual(self.run_command("power-selftest 1", b""), 2)

    def test_long_selftest_default_budget_is_600_seconds(self):
        self.assertEqual(self.run_command("power-selftest 30", b"", timeout=None), 2)
        self.assertGreaterEqual(self.last_elapsed, 600)

    def test_short_selftest_default_budget_is_120_seconds(self):
        self.assertEqual(self.run_command("power-selftest 1", b"", timeout=None), 2)
        self.assertGreaterEqual(self.last_elapsed, 120)

    def test_malformed_success_is_not_a_pass(self):
        self.assertEqual(self.run_command("power-selftest 1", b"[PowerTestResult] success=10\n"), 2)

    def test_requested_sleep_can_disconnect_usb(self):
        self.assertEqual(self.run_command("sleep", b"[DiagDone] sleep requested=1\n", True), 0)

    def test_rejected_sleep_does_not_hide_disconnect(self):
        self.assertEqual(self.run_command("sleep", b"[DiagDone] sleep requested=0\n", True), 1)

    def test_other_disconnect_is_failure(self):
        self.assertEqual(self.run_command("status", b"", True), 1)

    def test_feedback_needs_completed_successful_refresh(self):
        self.assertEqual(self.run_command("feedback-start", b"[FeedbackTest] started=1\n[ListeningFeedback] state=0 ok=1 error=0\n"), 0)
        self.assertEqual(self.run_command("feedback-cold", b"[FeedbackTest] cold_started=1\n[DiagDone] feedback-cold\n"), 2)
        self.assertEqual(self.run_command("feedback-cold", b"[ListeningFeedback] state=0 ok=0 error=3\n"), 2)
        self.assertEqual(self.run_command("feedback-start", b"[FeedbackTest] started=0\n[ListeningFeedback] state=0 ok=1 error=0\n"), 2)
        self.assertEqual(self.run_command("feedback-start", b"[ListeningFeedback] state=0 ok=1 error=0\n[FeedbackTest] started=1\n"), 2)

    def test_feedback_ack_does_not_finish_before_late_full_refresh(self):
        self.assertEqual(self.run_command("feedback-cold", b"[FeedbackTest] cold_started=1\n[DiagDone] feedback-cold\n",
                                         timeout="8", delayed_payload=b"[ListeningFeedback] state=0 ok=1 error=0\n"), 0)
        self.assertGreaterEqual(self.last_elapsed, 4)


if __name__ == "__main__":
    unittest.main()
