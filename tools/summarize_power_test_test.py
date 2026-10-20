import copy
import json
import unittest
from summarize_power_test import summarize


class SummaryTests(unittest.TestCase):
    def setUp(self):
        self.rows = [dict(rep=1, case=case, in_hz=16000, out_hz=16000,
                          setup_us=100 + case, bytes=200, captured_samples=100,
                          cleanup=True, wav=True, quiescent=True, overflow=0,
                          failed=False, session_failed=False, storage_full=False,
                          timeout=False, heap_internal=1000, psram=2000,
                          **{"pass": True}) for case in range(1, 9)]

    def text(self, rows=None, success=True):
        return "\n".join("[PowerTest] " + json.dumps(row) for row in (self.rows if rows is None else rows)) + (
            "\n[PowerTestResult] success=1\n" if success else "\n")

    def test_complete(self):
        result = summarize(self.text(), 1)
        self.assertEqual(result["cases"], 8)
        self.assertEqual(result["rates"][0]["codec_setup_us"]["median"], 104.5)
        self.assertEqual(result["rates"][0]["codec_setup_us"]["p95"], 108)

    def test_reject_incomplete_or_duplicate(self):
        for rows in (self.rows[:-1], self.rows + [self.rows[0]], self.rows[:-1] + [self.rows[0]]):
            with self.assertRaises(ValueError):
                summarize(self.text(rows), 1)

    def test_reject_missing_success(self):
        with self.assertRaises(ValueError):
            summarize(self.text(success=False), 1)

    def test_reject_failed_or_corrupt_row(self):
        for field, value in (("overflow", 1), ("cleanup", False), ("quiescent", False),
                             ("failed", True), ("captured_samples", 101), ("bytes", 0)):
            rows = copy.deepcopy(self.rows)
            rows[0][field] = value
            with self.assertRaises(ValueError):
                summarize(self.text(rows), 1)


if __name__ == "__main__":
    unittest.main()
