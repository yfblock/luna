# SPDX-License-Identifier: GPL-2.0
import sys
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from benchmark import check_baseline, percentile, report_value  # noqa: E402


class BenchmarkHelpersTest(unittest.TestCase):
    def test_percentile_uses_nearest_rank(self) -> None:
        values = [9, 1, 5, 3]
        self.assertEqual(percentile(values, 50), 3)
        self.assertEqual(percentile(values, 95), 9)

    def test_nested_report_value(self) -> None:
        self.assertEqual(report_value({"a": {"b": 7}}, "a.b"), 7)
        with self.assertRaises(ValueError):
            report_value({"a": {}}, "a.missing")

    def test_baseline_reports_all_comparison_types(self) -> None:
        report = {"network_backend": "socket", "metric": 10, "fixed": 2}
        baseline = {
            "network_backend": "socket",
            "minimums": {"metric": 11},
            "maximums": {"metric": 9},
            "equals": {"fixed": 3},
        }
        errors = check_baseline(report, baseline)
        self.assertEqual(len(errors), 3)

    def test_baseline_accepts_matching_report(self) -> None:
        report = {"network_backend": "socket", "metric": 10, "fixed": 2}
        baseline = {
            "network_backend": "socket",
            "minimums": {"metric": 9},
            "maximums": {"metric": 11},
            "equals": {"fixed": 2},
        }
        self.assertEqual(check_baseline(report, baseline), [])


if __name__ == "__main__":
    unittest.main()
