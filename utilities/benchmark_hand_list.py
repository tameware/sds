#!/usr/bin/env python3
"""Build single_dummy_solver and benchmark each deal in a hand list."""

from __future__ import annotations

import argparse
import shlex
import statistics
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BINARY = REPO_ROOT / "bazel-bin" / "single_dummy_solver"


def repo_root() -> Path:
    return REPO_ROOT


def default_solver_binary() -> Path:
    return DEFAULT_BINARY


def parse_deal_lines(path: Path) -> list[str]:
    deals: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if not stripped.startswith("--known "):
            raise ValueError(f"expected deal line starting with --known: {stripped!r}")
        deals.append(stripped)
    if not deals:
        raise ValueError(f"no deals found in {path}")
    return deals


def split_deal_args(deal_line: str) -> list[str]:
    return shlex.split(deal_line)


def split_benchmark_and_solver_argv(argv: list[str]) -> tuple[list[str], list[str]]:
    if "--" not in argv:
        return argv, []
    separator = argv.index("--")
    return argv[:separator], argv[separator + 1 :]


def shows_solver_output(extra_args: list[str]) -> bool:
    return "--diagram" in extra_args


def solver_command(
    binary: Path,
    deal_line: str,
    *,
    extra_args: list[str] | None = None,
) -> list[str]:
    args = split_deal_args(deal_line)
    if extra_args:
        args.extend(extra_args)
    return [str(binary), *args]


def build_solver(root: Path = REPO_ROOT) -> None:
    subprocess.run(
        ["bazelisk", "build", "//:single_dummy_solver"],
        cwd=root,
        check=True,
    )


def run_deal(
    binary: Path,
    deal_line: str,
    *,
    extra_args: list[str] | None = None,
) -> float:
    solver_extra = extra_args or []
    start = time.perf_counter()
    command = solver_command(binary, deal_line, extra_args=solver_extra)
    if shows_solver_output(solver_extra):
        subprocess.run(command, check=True, text=True)
    else:
        subprocess.run(
            command,
            check=True,
            capture_output=True,
            text=True,
        )
    return time.perf_counter() - start


def summarize_times(times: list[float]) -> dict[str, float | int]:
    return {
        "count": len(times),
        "min": min(times),
        "max": max(times),
        "mean": statistics.mean(times),
        "median": statistics.median(times),
    }


def format_timing_summary(stats: dict[str, float | int]) -> str:
    lines = [
        f"deals: {stats['count']}",
        f"min: {stats['min']:.1f} s",
        f"max: {stats['max']:.1f} s",
        f"mean: {stats['mean']:.1f} s",
        f"median: {stats['median']:.1f} s",
    ]
    return "\n".join(lines)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Build single_dummy_solver and benchmark each deal line from a "
            "generate_hand_list.py output file. Pass single_dummy_solver "
            "flags after --."
        ),
    )
    parser.add_argument(
        "hand_list",
        type=Path,
        help="path to listN_C_cards.txt",
    )
    parser.add_argument(
        "--binary",
        type=Path,
        default=DEFAULT_BINARY,
        help=f"path to single_dummy_solver (default: {DEFAULT_BINARY})",
    )
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="skip bazelisk build and use the existing binary",
    )
    benchmark_argv, solver_extra = split_benchmark_and_solver_argv(argv)
    args = parser.parse_args(benchmark_argv)
    args.solver_extra = solver_extra
    return args


def print_deal_separator() -> None:
    print("--", flush=True)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    if not args.skip_build:
        build_solver()
    deals = parse_deal_lines(args.hand_list)
    times = []
    for deal_line in deals:
        times.append(run_deal(args.binary, deal_line, extra_args=args.solver_extra))
        print_deal_separator()
    print(format_timing_summary(summarize_times(times)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
