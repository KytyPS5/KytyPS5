#!/usr/bin/env python3
"""Run one isolated graphics regression comparison."""

from __future__ import annotations

import argparse
from pathlib import Path

from graphics_regression import CaseSpec, run_case


def frame_ordinals(value: str) -> list[int]:
    try:
        frames = [int(part) for part in value.split(",")]
    except ValueError as error:
        raise argparse.ArgumentTypeError("frames must be comma-separated integers") from error
    if not frames:
        raise argparse.ArgumentTypeError("at least one frame is required")
    return frames


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-id", required=True)
    parser.add_argument("--emulator", required=True, type=Path)
    parser.add_argument("--game", required=True, type=Path)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--frames", required=True, type=frame_ordinals)
    parser.add_argument("--timeout-seconds", type=float, default=300.0)
    parser.add_argument("--save-raw", action="store_true")
    parser.add_argument("--reuse-work-dir", action="store_true")
    parser.add_argument("--allow-environment-mismatch", action="store_true")
    parser.add_argument("--launcher-arg", action="append", default=[])
    parser.add_argument("emulator_args", nargs=argparse.REMAINDER,
                        help="additional emulator arguments after --")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    emulator_args = args.emulator_args[1:] if args.emulator_args[:1] == ["--"] else args.emulator_args
    report = args.report.resolve()
    spec = CaseSpec(
        test_id=args.test_id,
        emulator=args.emulator.resolve(),
        game=args.game.resolve(),
        baseline=args.baseline.resolve(),
        report=report,
        work_dir=(args.work_dir or report.parent / f"{report.stem}_work").resolve(),
        frames=args.frames,
        timeout_seconds=args.timeout_seconds,
        save_raw=args.save_raw,
        reuse_work_dir=args.reuse_work_dir,
        allow_environment_mismatch=args.allow_environment_mismatch,
        launcher_args=tuple(args.launcher_arg),
        emulator_args=tuple(emulator_args),
    )
    try:
        result = run_case(spec)
    except (OSError, ValueError) as error:
        print(f"graphics regression: {error}")
        return 2
    print(f"graphics regression [{result.test_id}]: {result.message}")
    return result.exit_code


if __name__ == "__main__":
    raise SystemExit(main())
