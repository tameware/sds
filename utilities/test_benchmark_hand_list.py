#!/usr/bin/env python3
"""Tests for benchmark_hand_list.py."""

import tempfile
import unittest
from pathlib import Path
from unittest import mock

from benchmark_hand_list import (
    default_solver_binary,
    format_deal_timing,
    format_timing_summary,
    parse_deal_lines,
    repo_root,
    shows_solver_output,
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

    def test_format_timing_summary_uses_milliseconds(self):
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
        self.assertIn("min: 40 ms", summary)
        self.assertIn("max: 12345 ms", summary)
        self.assertIn("mean: 4567 ms", summary)
        self.assertIn("median: 3999 ms", summary)

    def test_format_deal_timing_uses_milliseconds(self):
        self.assertEqual(format_deal_timing(12.345), "12345 ms")
        self.assertEqual(format_deal_timing(0.04), "40 ms")

    def test_default_solver_binary_points_at_bazel_bin(self):
        self.assertEqual(
            default_solver_binary(),
            repo_root() / "bazel-bin" / "single_dummy_solver",
        )

    def test_shows_solver_output_when_diagram_requested(self):
        self.assertTrue(shows_solver_output(["--diagram"]))
        self.assertFalse(shows_solver_output(["--threads", "4"]))

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

    def test_run_deal_forwards_solver_output_when_diagram_requested(self):
        from benchmark_hand_list import run_deal

        binary = Path("/tmp/single_dummy_solver")
        with mock.patch("benchmark_hand_list.subprocess.run") as run_mock:
            run_mock.return_value = mock.Mock(returncode=0)
            with mock.patch("benchmark_hand_list.time.perf_counter", side_effect=[1.0, 2.0]):
                run_deal(
                    binary,
                    "--known A... --known-seat S",
                    extra_args=["--diagram"],
                )
        self.assertNotIn("capture_output", run_mock.call_args.kwargs)
        self.assertEqual(run_mock.call_args.args[0][-1], "--diagram")

    def test_solver_command_appends_extra_solver_args(self):
        from benchmark_hand_list import solver_command

        command = solver_command(
            Path("/tmp/single_dummy_solver"),
            "--known A... --known-seat S --dummy K... --dummy-seat N",
            extra_args=["--threads", "4", "--goal", "8"],
        )
        self.assertEqual(command[-4:], ["--threads", "4", "--goal", "8"])

    def test_parse_args_accepts_benchmark_flags_only(self):
        from benchmark_hand_list import parse_args

        args = parse_args(["hands/list3_4_cards.txt", "--skip-build"])
        self.assertTrue(args.skip_build)
        self.assertFalse(args.per_deal)
        self.assertEqual(args.solver_extra, [])

    def test_parse_args_accepts_per_deal_flag(self):
        from benchmark_hand_list import parse_args

        args = parse_args(
            ["hands/list3_4_cards.txt", "--skip-build", "--per-deal"]
        )
        self.assertTrue(args.per_deal)

    def test_parse_args_collects_solver_flags_after_double_dash(self):
        from benchmark_hand_list import parse_args

        args = parse_args(
            [
                "hands/list3_4_cards.txt",
                "--skip-build",
                "--",
                "--search-threads",
                "4",
                "--goal",
                "8",
                "--diagram",
            ]
        )
        self.assertTrue(args.skip_build)
        self.assertEqual(
            args.solver_extra,
            ["--search-threads", "4", "--goal", "8", "--diagram"],
        )

    def test_run_deal_forwards_extra_solver_args(self):
        from benchmark_hand_list import run_deal

        binary = Path("/tmp/single_dummy_solver")
        with mock.patch("benchmark_hand_list.subprocess.run") as run_mock:
            run_mock.return_value = mock.Mock(returncode=0)
            with mock.patch("benchmark_hand_list.time.perf_counter", side_effect=[1.0, 2.0]):
                run_deal(
                    binary,
                    "--known A... --known-seat S",
                    extra_args=["--search-threads", "0"],
                )
        self.assertEqual(run_mock.call_args.args[0][-2:], ["--search-threads", "0"])


    def test_main_prints_separator_after_each_deal(self):
        from benchmark_hand_list import main

        with mock.patch("benchmark_hand_list.build_solver"), mock.patch(
            "benchmark_hand_list.run_deal",
            return_value=1.0,
        ), mock.patch(
            "benchmark_hand_list.parse_deal_lines",
            return_value=["--known A...", "--known B..."],
        ), mock.patch(
            "benchmark_hand_list.print_deal_separator",
        ) as separator_mock:
            main(["hands/list3_4_cards.txt", "--skip-build"])

        self.assertEqual(separator_mock.call_count, 2)

    def test_main_omits_per_deal_times_by_default(self):
        from benchmark_hand_list import main

        with mock.patch("benchmark_hand_list.build_solver"), mock.patch(
            "benchmark_hand_list.run_deal",
            side_effect=[1.25, 2.5],
        ), mock.patch(
            "benchmark_hand_list.parse_deal_lines",
            return_value=["--known A...", "--known B..."],
        ), mock.patch(
            "benchmark_hand_list.print_deal_separator",
        ), mock.patch("builtins.print") as print_mock:
            main(["hands/list3_4_cards.txt", "--skip-build"])

        printed_lines = [
            str(call.args[0]) for call in print_mock.call_args_list if call.args
        ]
        self.assertEqual(len(printed_lines), 1)
        self.assertIn("deals: 2", printed_lines[0])

    def test_main_prints_per_deal_times_when_requested(self):
        from benchmark_hand_list import main

        with mock.patch("benchmark_hand_list.build_solver"), mock.patch(
            "benchmark_hand_list.run_deal",
            side_effect=[1.25, 2.5],
        ), mock.patch(
            "benchmark_hand_list.parse_deal_lines",
            return_value=["--known A...", "--known B..."],
        ), mock.patch(
            "benchmark_hand_list.print_deal_separator",
        ), mock.patch("builtins.print") as print_mock:
            main(["hands/list3_4_cards.txt", "--skip-build", "--per-deal"])

        printed_lines = [
            str(call.args[0]) for call in print_mock.call_args_list if call.args
        ]
        self.assertEqual(printed_lines[0], "1250 ms")
        self.assertEqual(printed_lines[1], "2500 ms")
        self.assertIn("deals: 2", printed_lines[2])

    def test_print_deal_separator_writes_double_dash_line(self):
        from benchmark_hand_list import print_deal_separator

        with mock.patch("builtins.print") as print_mock:
            print_deal_separator()
        print_mock.assert_called_once_with("--", flush=True)


if __name__ == "__main__":
    unittest.main()
