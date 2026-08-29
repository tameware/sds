#!/usr/bin/env python3
"""Tests for benchmark_hand_list.py."""

import tempfile
import unittest
from pathlib import Path
from unittest import mock

from benchmark_hand_list import (
    default_solver_binary,
    format_timing_summary,
    parse_deal_lines,
    repo_root,
    split_deal_args,
    summarize_times,
)


class BenchmarkHandListTest(unittest.TestCase):
    def test_parse_deal_lines_skips_comments_and_blank_lines(self):
        text = """# header
# 2 deals, 3 cards per hand, seed=31

--known A... --known-seat S --dummy K... --dummy-seat N --available Q.2.. --trump S
--known Q... --known-seat S --dummy J... --dummy-seat N --available A.2.. --trump H
"""
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as handle:
            handle.write(text)
            path = Path(handle.name)
        try:
            lines = parse_deal_lines(path)
        finally:
            path.unlink()
        self.assertEqual(len(lines), 2)
        self.assertTrue(lines[0].startswith("--known "))

    def test_split_deal_args_parses_cli_flags(self):
        line = (
            "--known K98.3..A --known-seat S --dummy AJT.2..K --dummy-seat N "
            "--available Q765...765432 --trump S"
        )
        self.assertEqual(
            split_deal_args(line)[:2],
            ["--known", "K98.3..A"],
        )

    def test_summarize_times_computes_basic_statistics(self):
        stats = summarize_times([1.0, 2.0, 3.0, 4.0])
        self.assertEqual(stats["min"], 1.0)
        self.assertEqual(stats["max"], 4.0)
        self.assertEqual(stats["mean"], 2.5)
        self.assertEqual(stats["median"], 2.5)

    def test_format_timing_summary_uses_one_decimal_place(self):
        summary = format_timing_summary(
            {
                "count": 3,
                "min": 0.04,
                "max": 12.345,
                "mean": 4.567,
                "median": 3.999,
            }
        )
        self.assertIn("deals: 3", summary)
        self.assertIn("min: 0.0 s", summary)
        self.assertIn("max: 12.3 s", summary)
        self.assertIn("mean: 4.6 s", summary)
        self.assertIn("median: 4.0 s", summary)

    def test_default_solver_binary_points_at_bazel_bin(self):
        self.assertEqual(
            default_solver_binary(),
            repo_root() / "bazel-bin" / "single_dummy_solver",
        )

    def test_run_deal_times_subprocess(self):
        from benchmark_hand_list import run_deal

        binary = Path("/tmp/single_dummy_solver")
        with mock.patch("benchmark_hand_list.subprocess.run") as run_mock:
            run_mock.return_value = mock.Mock(returncode=0)
            with mock.patch("benchmark_hand_list.time.perf_counter", side_effect=[1.0, 3.25]):
                elapsed = run_deal(binary, "--known A... --known-seat S")
        self.assertAlmostEqual(elapsed, 2.25)
        run_mock.assert_called_once()
        self.assertTrue(run_mock.call_args.kwargs["capture_output"])

    def test_run_deal_with_detail_forwards_solver_output(self):
        from benchmark_hand_list import run_deal

        binary = Path("/tmp/single_dummy_solver")
        with mock.patch("benchmark_hand_list.subprocess.run") as run_mock:
            run_mock.return_value = mock.Mock(returncode=0)
            with mock.patch("benchmark_hand_list.time.perf_counter", side_effect=[1.0, 2.0]):
                run_deal(binary, "--known A... --known-seat S", detail=True)
        self.assertNotIn("capture_output", run_mock.call_args.kwargs)
        self.assertEqual(run_mock.call_args.args[0][-1], "--diagram")

    def test_solver_command_appends_diagram_when_detail_enabled(self):
        from benchmark_hand_list import solver_command

        command = solver_command(
            Path("/tmp/single_dummy_solver"),
            "--known A... --known-seat S --dummy K... --dummy-seat N",
            detail=True,
        )
        self.assertEqual(command[0], "/tmp/single_dummy_solver")
        self.assertEqual(command[-1], "--diagram")
        self.assertNotIn("--diagram", command[:-1])

    def test_parse_args_accepts_detail_flag(self):
        from benchmark_hand_list import parse_args

        args = parse_args(["hands/list3_4_cards.txt", "--detail"])
        self.assertTrue(args.detail)

    def test_solver_command_appends_goal_when_set(self):
        from benchmark_hand_list import solver_command

        command = solver_command(
            Path("/tmp/single_dummy_solver"),
            "--known A... --known-seat S --dummy K... --dummy-seat N",
            goal=4,
        )
        self.assertEqual(command[-2:], ["--goal", "4"])

    def test_run_deal_with_goal_forwards_goal_flag(self):
        from benchmark_hand_list import run_deal

        binary = Path("/tmp/single_dummy_solver")
        with mock.patch("benchmark_hand_list.subprocess.run") as run_mock:
            run_mock.return_value = mock.Mock(returncode=0)
            with mock.patch("benchmark_hand_list.time.perf_counter", side_effect=[1.0, 2.0]):
                run_deal(binary, "--known A... --known-seat S", goal=4)
        self.assertEqual(run_mock.call_args.args[0][-2:], ["--goal", "4"])

    def test_parse_args_accepts_goal_flag(self):
        from benchmark_hand_list import parse_args

        args = parse_args(["hands/list3_4_cards.txt", "--goal", "4"])
        self.assertEqual(args.goal, 4)


if __name__ == "__main__":
    unittest.main()
